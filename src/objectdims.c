/*
 * objectdims.c - "Object Dimensions" shader node.
 *
 * Reports the bounding box and volume of an object as node outputs you can wire
 * into any other input (scale, tiling, displacement amplitude, falloff distance,
 * etc.). Think of it as the missing "size" pins on the Object Info node.
 *
 *   inputs : none. The measured object is the surface's own object (sg->itemID),
 *            or the Reference Object picked on the panel.
 *   outputs: Size, Center, Min, Max   (vectors, in the selected space)
 *            Top, Bottom, Left, Right, Front, Back  (face-centre positions)
 *            Center of Mass, Position in Box, Sphere Center  (vectors)
 *            Diagonal, Box Volume, Mesh Volume, Surface Area, Sphere Radius,
 *            Point Count, Polygon Count  (scalars)
 *
 * Registered for every node editor (NCON_ALL). Where there is no surface object
 * (environment, camera, light graphs) set a Reference Object; with neither, the
 * outputs are zero.
 *
 * The six side outputs are the centre point of each bounding-box face, following
 * LightWave's viewport axis convention: Top = +Y, Bottom = -Y, Right = +X,
 * Left = -X, Front = -Z (the face the Front viewport looks at), Back = +Z.
 *
 * Position in Box is where the point being shaded sits inside the measured box,
 * 0 to 1 per axis (0.5 on an axis where the box is flat), so a gradient wired to it
 * fits any object. Center of Mass is the volume centroid of a closed mesh (the box
 * centre when the mesh encloses no volume). Sphere Center / Sphere Radius give the
 * smallest sphere about the object's own box centre that holds every vertex; in
 * World Space the radius is scaled by the largest axis scale (exact for uniform
 * scale, an upper bound for non-uniform).
 *
 * Space toggle:
 *   Object (local) - the object's modelled dimensions in object units.
 *   World          - the axis-aligned bounding box in world units, i.e. the box's
 *                    footprint on the world grid after the item's transform.
 *
 * Why a render-time cache?  Mesh Volume (and the exact object-space box) require
 * scanning the mesh via LWObjectInfo->meshInfo(). That global is only valid in the
 * single-threaded render-setup phase (init / newTime) - during multithreaded
 * surface shading it returns NULL, which is why an earlier lazy-in-Evaluate version
 * reported Mesh Volume = 0 every time. So we walk every object once per frame in
 * newTime (where meshInfo works), compute each object's local box, volume and
 * centre of mass, read its object-to-world transform, and stash them in a small
 * file-static table keyed by itemID. Evaluate just looks up the item - a plain read
 * of a table that is only written single-threaded.
 *
 * Coordinate spaces:
 *   - LWObjectInfo->bounds() returns the box in OBJECT space (Layout-verified), and
 *     LWShadingGeometry.toWorld carries no translation. World values therefore come
 *     from LWItemInfo->param(LWIP_TRANSFORM): the 8 local box corners transformed
 *     and re-boxed, the centre of mass transformed, and the volume scaled by |det|
 *     of the transform's linear part.
 *   - LWIP_TRANSFORM is row-vector with the translation in row 3:
 *     world[i] = p[0]*M[0][i] + p[1]*M[1][i] + p[2]*M[2][i] + M[3][i].
 */

#include <lwserver.h>
#include <lwnodes.h>
#include <lwrender.h>
#include <lwmeshes.h>
#include <lwhost.h>
#include <lwxpanel.h>
#include <lwio.h>
#include <lwhandler.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* LWItemInfo calls are not safe from several render threads at once; the rare
   Evaluate-time calls (cold cache, see ODData) are serialised with this lock. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static SRWLOCK od_lockv = SRWLOCK_INIT;
static void od_lock(void)   { AcquireSRWLockExclusive(&od_lockv); }
static void od_unlock(void) { ReleaseSRWLockExclusive(&od_lockv); }
#else
#include <pthread.h>
static pthread_mutex_t od_lockv = PTHREAD_MUTEX_INITIALIZER;
static void od_lock(void)   { pthread_mutex_lock(&od_lockv); }
static void od_unlock(void) { pthread_mutex_unlock(&od_lockv); }
#endif

static LWNodeOutputFuncs *od_out;
static LWNodeInputFuncs  *od_in;
static LWObjectInfo      *od_obj;
static LWItemInfo        *od_item;
static LWTimeInfo        *od_time;
static LWXPanelFuncs     *od_xpan;
static LWInstUpdate      *od_iupd;

/* ---- per-object measurements (cache entry, and the per-instance memo) ---- */

typedef struct {
    LWItemID item;
    double   mn[3], mx[3];   /* object-space bounding box */
    double   vol;            /* mesh volume in object units^3 */
    double   area;           /* object-space surface area in object units^2 */
    double   com[3];         /* object-space centre of mass */
    double   rad;            /* bounding sphere radius about the box centre */
    int      npnts, npols;   /* raw mesh point / polygon counts */
    double   M[4][4];        /* object -> world (LWIP_TRANSFORM) */
    double   Mi[4][4];       /* world -> object (LWIP_W_TRANSFORM) */
} VolEntry;

static VolEntry *g_cache = NULL;
static int       g_count = 0;
static int       g_cap   = 0;
static LWTime    g_time  = 0.0;
static int       g_haveTime = 0;

#define OD_NAMELEN 256

typedef struct st_ODData {
    NodeID        nodeID;
    NodeOutputID  out_size, out_center, out_min, out_max;
    NodeOutputID  out_top, out_bottom, out_left, out_right, out_front, out_back;
    NodeOutputID  out_com, out_posbox, out_sphc, out_sphr;
    NodeOutputID  out_diag, out_boxvol, out_meshvol;
    NodeOutputID  out_area, out_npnts, out_npols;
    int           world;      /* 0 = object/local space, 1 = world space */

    /* Reference Object: measured instead of the surface's own object. Saved by name;
       refID is resolved single-threaded (newTime, panel) or under od_lock. */
    char          refName[OD_NAMELEN];   /* "" = the surface's own object */
    LWItemID      refID;
    LWTime        refTriedTime;
    int           refTried;

    /* Per-instance memo of a lazy mesh scan. When the render-time cache (g_cache)
       is cold - e.g. this node drives a ProGeo graph rather than shading a surface,
       so rend->newTime/init never populated it - Evaluate would otherwise re-scan
       the entire mesh on every single sample. The measured item is constant for a
       node instance, so we scan once per frame and cache the result here. It is
       dropped on render init, newTime and any change of time, so an edited mesh is
       picked up. (Concurrent first-time writes across render threads are benign:
       every thread computes identical values.) */
    VolEntry      memo;
    LWTime        memo_time;
    int           memo_have;  /* 0 = not scanned, 1 = scanned OK, 2 = scanned empty */
} ODData;

#define ID_WLD LWID_('W','L','D',' ')
#define ID_REF LWID_('R','E','F',' ')
static LWBlockIdent odblk[] = { {ID_WLD,"wld"}, {ID_REF,"ref"}, {0} };

/* ---- small maths ---- */

static void od_identity(double M[4][4])
{
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) M[i][j] = (i == j) ? 1.0 : 0.0;
}

static void od_xform(const double M[4][4], const double p[3], double out[3])
{
    int i;
    for (i = 0; i < 3; i++)
        out[i] = p[0]*M[0][i] + p[1]*M[1][i] + p[2]*M[2][i] + M[3][i];
}

/* |det| of the 3x3 linear part of a 4x4 (magnitude is layout-independent). */
static double od_linearDet(const double M[4][4])
{
    double d =
        M[0][0]*(M[1][1]*M[2][2] - M[1][2]*M[2][1])
      - M[0][1]*(M[1][0]*M[2][2] - M[1][2]*M[2][0])
      + M[0][2]*(M[1][0]*M[2][1] - M[1][1]*M[2][0]);
    return fabs(d);
}

/* World AABB of an object-space box: transform the 8 corners and re-box them. */
static void od_worldBox(const double M[4][4], const double mn[3], const double mx[3],
                        double wmn[3], double wmx[3])
{
    int c, k;
    for (k = 0; k < 3; k++) { wmn[k] = 1e30; wmx[k] = -1e30; }
    for (c = 0; c < 8; c++) {
        double p[3], w[3];
        p[0] = (c & 1) ? mx[0] : mn[0];
        p[1] = (c & 2) ? mx[1] : mn[1];
        p[2] = (c & 4) ? mx[2] : mn[2];
        od_xform(M, p, w);
        for (k = 0; k < 3; k++) {
            if (w[k] < wmn[k]) wmn[k] = w[k];
            if (w[k] > wmx[k]) wmx[k] = w[k];
        }
    }
}

/* Object <-> world transforms of an item at time t. Not render-thread safe. */
static void od_getXform(LWItemID item, LWTime t, double M[4][4], double Mi[4][4])
{
    od_identity(M);
    od_identity(Mi);
    if (!od_item || !item) return;
    od_item->param(item, LWIP_TRANSFORM,   t, &M[0][0]);
    od_item->param(item, LWIP_W_TRANSFORM, t, &Mi[0][0]);
}

/* ---- mesh scan: object-space bbox + signed-tetra volume + centre of mass ---- */

typedef struct {
    LWMeshInfoID mesh;
    double mn[3], mx[3];
    double vol;              /* signed */
    double area;
    double cm[3];            /* sum of signed tetra volume x tetra centroid */
} ScanCtx;

static int od_polyScan(void *ud, LWPolID pol)
{
    ScanCtx *c = (ScanCtx *)ud;
    int n = c->mesh->polSize(c->mesh, pol);
    LWID  ptype = c->mesh->polType(c->mesh, pol);
    LWFVector fp;
    double v0[3];
    int i, k;

    if (n < 1) return 0;

    /* bounding box uses every vertex of every polygon (object-space positions) */
    for (i = 0; i < n; i++) {
        c->mesh->pntOtherPos(c->mesh, c->mesh->polVertex(c->mesh, pol, i), fp);
        for (k = 0; k < 3; k++) {
            double val = fp[k];
            if (val < c->mn[k]) c->mn[k] = val;
            if (val > c->mx[k]) c->mx[k] = val;
        }
    }

    /* volume only from true faces with 3+ vertices (skip curves, 1/2-point polys) */
    if (n < 3 || ptype != LWPOLTYPE_FACE) return 0;

    c->mesh->pntOtherPos(c->mesh, c->mesh->polVertex(c->mesh, pol, 0), fp);
    v0[0] = fp[0]; v0[1] = fp[1]; v0[2] = fp[2];
    for (i = 1; i < n - 1; i++) {
        double a[3], b[3], cx, cy, cz, ex, ey, ez, fx, fy, fz, tx, ty, tz, tv;
        c->mesh->pntOtherPos(c->mesh, c->mesh->polVertex(c->mesh, pol, i), fp);
        a[0] = fp[0]; a[1] = fp[1]; a[2] = fp[2];
        c->mesh->pntOtherPos(c->mesh, c->mesh->polVertex(c->mesh, pol, i + 1), fp);
        b[0] = fp[0]; b[1] = fp[1]; b[2] = fp[2];
        /* signed volume of tetra (origin, v0, a, b) = v0 . (a x b) / 6 */
        cx = a[1]*b[2] - a[2]*b[1];
        cy = a[2]*b[0] - a[0]*b[2];
        cz = a[0]*b[1] - a[1]*b[0];
        tv = (v0[0]*cx + v0[1]*cy + v0[2]*cz) / 6.0;
        c->vol += tv;
        /* centre of mass: tetra centroid (origin + v0 + a + b) / 4, weighted by tv */
        for (k = 0; k < 3; k++) c->cm[k] += tv * (v0[k] + a[k] + b[k]) * 0.25;
        /* triangle-fan area: 1/2 |(a-v0) x (b-v0)| */
        ex = a[0]-v0[0]; ey = a[1]-v0[1]; ez = a[2]-v0[2];
        fx = b[0]-v0[0]; fy = b[1]-v0[1]; fz = b[2]-v0[2];
        tx = ey*fz - ez*fy;
        ty = ez*fx - ex*fz;
        tz = ex*fy - ey*fx;
        c->area += 0.5 * sqrt(tx*tx + ty*ty + tz*tz);
    }
    return 0;
}

/* second pass: farthest vertex from the box centre (the bounding sphere radius) */
typedef struct {
    LWMeshInfoID mesh;
    double c[3];
    double maxd2;
} RadCtx;

static int od_radScan(void *ud, LWPolID pol)
{
    RadCtx *r = (RadCtx *)ud;
    int i, n = r->mesh->polSize(r->mesh, pol);
    LWFVector fp;
    for (i = 0; i < n; i++) {
        double dx, dy, dz, d2;
        r->mesh->pntOtherPos(r->mesh, r->mesh->polVertex(r->mesh, pol, i), fp);
        dx = fp[0] - r->c[0]; dy = fp[1] - r->c[1]; dz = fp[2] - r->c[2];
        d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > r->maxd2) r->maxd2 = d2;
    }
    return 0;
}

/* Measure one item in object space (box, volume, area, centre of mass, bounding
   sphere, counts).
   Leaves e->M / e->Mi alone. Returns 1 on success. Needs meshInfo, so it only
   works single-threaded or outside surface shading. */
static int od_scanItem(LWItemID item, VolEntry *e)
{
    LWMeshInfoID mesh;
    ScanCtx ctx;
    int k, np = 0, ng = 0;

    if (!od_obj || !item) return 0;
    mesh = od_obj->meshInfo(item, 0);
    if (!mesh) return 0;

    memset(&ctx, 0, sizeof ctx);
    ctx.mesh = mesh;
    for (k = 0; k < 3; k++) { ctx.mn[k] = 1e30; ctx.mx[k] = -1e30; }
    mesh->scanPolys(mesh, (LWPolScanFunc *)od_polyScan, &ctx);
    if (ctx.mx[0] < ctx.mn[0]) {            /* nothing scanned */
        if (mesh->destroy) mesh->destroy(mesh);
        return 0;
    }
    {
        RadCtx r;
        r.mesh = mesh;
        for (k = 0; k < 3; k++) r.c[k] = 0.5 * (ctx.mn[k] + ctx.mx[k]);
        r.maxd2 = 0.0;
        mesh->scanPolys(mesh, (LWPolScanFunc *)od_radScan, &r);
        e->rad = sqrt(r.maxd2);
    }
    if (mesh->numPoints)   np = mesh->numPoints(mesh);
    if (mesh->numPolygons) ng = mesh->numPolygons(mesh);
    if (mesh->destroy) mesh->destroy(mesh);

    e->item = item;
    for (k = 0; k < 3; k++) { e->mn[k] = ctx.mn[k]; e->mx[k] = ctx.mx[k]; }
    e->vol   = fabs(ctx.vol);
    e->area  = ctx.area;
    e->npnts = np;
    e->npols = ng;
    /* the signed sums give the centroid whichever way the mesh is wound; an open
       or flat mesh encloses no volume, so fall back to the box centre */
    {
        double ext = 0.0;
        for (k = 0; k < 3; k++) ext = fmax(ext, ctx.mx[k] - ctx.mn[k]);
        if (fabs(ctx.vol) > 1e-9 * ext * ext * ext && ext > 0.0)
            for (k = 0; k < 3; k++) e->com[k] = ctx.cm[k] / ctx.vol;
        else
            for (k = 0; k < 3; k++) e->com[k] = 0.5 * (ctx.mn[k] + ctx.mx[k]);
    }
    return 1;
}

/* Rebuild the whole-scene cache at time t. Single-threaded (init/newTime). */
static void od_rebuildCache(LWTime t)
{
    LWItemID item;

    g_count = 0;
    if (!od_item || !od_obj) return;

    for (item = od_item->first(LWI_OBJECT, NULL); item; item = od_item->next(item)) {
        VolEntry e;
        if (!od_scanItem(item, &e)) continue;
        od_getXform(item, t, e.M, e.Mi);
        if (g_count >= g_cap) {
            int ncap = g_cap ? g_cap * 2 : 16;
            VolEntry *n = (VolEntry *)realloc(g_cache, ncap * sizeof(VolEntry));
            if (!n) break;
            g_cache = n; g_cap = ncap;
        }
        g_cache[g_count++] = e;
    }
}

static VolEntry *od_lookup(LWItemID item)
{
    int i;
    for (i = 0; i < g_count; i++)
        if (g_cache[i].item == item) return &g_cache[i];
    return NULL;
}

/* Object item with this name, or NULL. Not render-thread safe. */
static LWItemID od_findObject(const char *name)
{
    LWItemID item;
    if (!od_item || !name || !name[0]) return NULL;
    for (item = od_item->first(LWI_OBJECT, NULL); item; item = od_item->next(item)) {
        const char *n = od_item->name(item);
        if (n && !strcmp(n, name)) return item;
    }
    return NULL;
}

static LWTime od_now(void) { return od_time ? od_time->time : g_time; }

/* ---- render lifecycle (single-threaded; meshInfo and param are valid here) ---- */

static void od_resolveRef(ODData *d, LWTime t)
{
    d->refID = d->refName[0] ? od_findObject(d->refName) : NULL;
    d->refTried = 1;
    d->refTriedTime = t;
}

XCALL_(static LWError) ODRendInit(LWInstance inst, int mode)
{
    ODData *d = (ODData *)inst;
    LWTime t = od_now();
    (void)mode;
    od_rebuildCache(t);
    g_haveTime = 0;          /* let the first newTime refresh for the frame's pose */
    if (d) { od_resolveRef(d, t); d->memo_have = 0; }
    return NULL;
}
XCALL_(static void) ODRendCleanup(LWInstance inst)
{
    (void)inst;
    g_haveTime = 0;          /* keep the buffer; force a rebuild next render */
}
XCALL_(static LWError) ODNewTime(LWInstance inst, LWFrame frame, LWTime t)
{
    ODData *d = (ODData *)inst;
    (void)frame;
    if (!g_haveTime || t != g_time) {
        od_rebuildCache(t);
        g_time = t;
        g_haveTime = 1;
    }
    if (d) { od_resolveRef(d, t); d->memo_have = 0; }
    return NULL;
}

/* ---- node lifecycle ---- */

XCALL_(static LWInstance) ODCreate(void *priv, void *context, LWError *err)
{
    ODData *d = (ODData *)calloc(1, sizeof(ODData));
    (void)priv; (void)err;
    if (!d) return NULL;
    d->nodeID = (NodeID)context;
    d->world  = 1;              /* default to world (what the user asked for) */
    d->out_size    = od_out->create(d->nodeID, NOT_VECTOR, "Size");
    d->out_center  = od_out->create(d->nodeID, NOT_VECTOR, "Center");
    d->out_min     = od_out->create(d->nodeID, NOT_VECTOR, "Min");
    d->out_max     = od_out->create(d->nodeID, NOT_VECTOR, "Max");
    d->out_top     = od_out->create(d->nodeID, NOT_VECTOR, "Top");
    d->out_bottom  = od_out->create(d->nodeID, NOT_VECTOR, "Bottom");
    d->out_left    = od_out->create(d->nodeID, NOT_VECTOR, "Left");
    d->out_right   = od_out->create(d->nodeID, NOT_VECTOR, "Right");
    d->out_front   = od_out->create(d->nodeID, NOT_VECTOR, "Front");
    d->out_back    = od_out->create(d->nodeID, NOT_VECTOR, "Back");
    d->out_com     = od_out->create(d->nodeID, NOT_VECTOR, "Center of Mass");
    d->out_posbox  = od_out->create(d->nodeID, NOT_VECTOR, "Position in Box");
    d->out_sphc    = od_out->create(d->nodeID, NOT_VECTOR, "Sphere Center");
    d->out_sphr    = od_out->create(d->nodeID, NOT_SCALAR, "Sphere Radius");
    d->out_diag    = od_out->create(d->nodeID, NOT_SCALAR, "Diagonal");
    d->out_boxvol  = od_out->create(d->nodeID, NOT_SCALAR, "Box Volume");
    d->out_meshvol = od_out->create(d->nodeID, NOT_SCALAR, "Mesh Volume");
    d->out_area    = od_out->create(d->nodeID, NOT_SCALAR, "Surface Area");
    d->out_npnts   = od_out->create(d->nodeID, NOT_SCALAR, "Point Count");
    d->out_npols   = od_out->create(d->nodeID, NOT_SCALAR, "Polygon Count");
    return d;
}
XCALL_(static void) ODDestroy(LWInstance inst) { free(inst); }
XCALL_(static LWError) ODCopy(LWInstance a, LWInstance b)
{
    ODData *da = (ODData *)a, *db = (ODData *)b;
    ODData keep = *da;      /* the node ID and output pins belong to the target */
    *da = *db;
    da->nodeID = keep.nodeID;
    da->out_size = keep.out_size; da->out_center = keep.out_center;
    da->out_min = keep.out_min;   da->out_max = keep.out_max;
    da->out_top = keep.out_top;   da->out_bottom = keep.out_bottom;
    da->out_left = keep.out_left; da->out_right = keep.out_right;
    da->out_front = keep.out_front; da->out_back = keep.out_back;
    da->out_com = keep.out_com;   da->out_posbox = keep.out_posbox;
    da->out_sphc = keep.out_sphc; da->out_sphr = keep.out_sphr;
    da->out_diag = keep.out_diag; da->out_boxvol = keep.out_boxvol;
    da->out_meshvol = keep.out_meshvol;
    da->out_area = keep.out_area; da->out_npnts = keep.out_npnts;
    da->out_npols = keep.out_npols;
    da->memo_have = 0;          /* force a fresh scan for this instance's own object */
    return NULL;
}
XCALL_(static LWError) ODLoad(LWInstance inst, const LWLoadState *ls)
{
    ODData *d = (ODData *)inst;
    LWID id;
    while ((id = LWLOAD_FIND(ls, odblk))) {
        if (id == ID_WLD) LWLOAD_I4(ls, &d->world, 1);
        else if (id == ID_REF) {
            LWLOAD_STR(ls, d->refName, OD_NAMELEN);
            d->refID = NULL;
            d->refTried = 0;    /* resolved once the scene's items exist */
        }
        LWLOAD_END(ls);
    }
    return NULL;
}
XCALL_(static LWError) ODSave(LWInstance inst, const LWSaveState *ss)
{
    ODData *d = (ODData *)inst;
    LWSAVE_BEGIN(ss, &odblk[0], 0); LWSAVE_I4(ss, &d->world, 1); LWSAVE_END(ss);
    LWSAVE_BEGIN(ss, &odblk[1], 0); LWSAVE_STR(ss, d->refName); LWSAVE_END(ss);
    return NULL;
}
XCALL_(static const char *) ODDescribe(LWInstance inst) { (void)inst; return "Object Dimensions"; }

/* Measurements of item at time t, from the render-time cache or, when that is cold,
   from the per-instance memo (scanning once per frame). NULL if unavailable. */
static const VolEntry *od_measure(ODData *d, LWItemID item, LWTime t)
{
    VolEntry *e = od_lookup(item);
    if (e) return e;
    if (d->memo_have && d->memo.item == item && d->memo_time == t)
        return d->memo_have == 1 ? &d->memo : NULL;
    {
        VolEntry s;
        int ok;
        memset(&s, 0, sizeof s);
        od_lock();
        ok = od_scanItem(item, &s);
        if (ok) od_getXform(item, t, s.M, s.Mi);
        od_unlock();
        s.item = item;
        d->memo = s;
        d->memo_time = t;
        d->memo_have = ok ? 1 : 2;   /* 2 = tried-and-empty, don't retry this frame */
        return ok ? &d->memo : NULL;
    }
}

XCALL_(static void) ODEvaluate(LWInstance inst, LWShadingGeometry *sg, NodeOutputID outID, NodeValue val)
{
    ODData *d = (ODData *)inst;
    double mn[3], mx[3], size[3], center[3], com[3], pos[3], pt[3], sphc[3];
    double boxvol, meshvol, diag, area, npntsd, npolsd, sphr = -1.0;
    const VolEntry *e;
    LWItemID target;
    LWTime t = od_now();
    int k;

    /* Reference Object: resolve lazily when no newTime has (ProGeo graphs). */
    if (d->refName[0] && !d->refID && (!d->refTried || d->refTriedTime != t)) {
        od_lock();
        od_resolveRef(d, t);
        od_unlock();
    }
    target = (d->refName[0] && d->refID) ? d->refID : sg->itemID;
    e = od_measure(d, target, t);

    if (e) {
        double lc[3];
        for (k = 0; k < 3; k++) lc[k] = 0.5 * (e->mn[k] + e->mx[k]);
        if (d->world) {
            double s = 0.0;
            od_worldBox(e->M, e->mn, e->mx, mn, mx);
            od_xform(e->M, e->com, com);
            od_xform(e->M, lc, sphc);
            meshvol = e->vol * od_linearDet(e->M);
            /* largest axis scale: exact for uniform scale, an upper bound otherwise */
            for (k = 0; k < 3; k++)
                s = fmax(s, sqrt(e->M[k][0]*e->M[k][0] + e->M[k][1]*e->M[k][1] + e->M[k][2]*e->M[k][2]));
            sphr = e->rad * s;
        } else {
            for (k = 0; k < 3; k++) {
                mn[k] = e->mn[k]; mx[k] = e->mx[k]; com[k] = e->com[k]; sphc[k] = lc[k];
            }
            meshvol = e->vol;
            sphr = e->rad;
        }
        /* Surface Area is a modelled (object-space) property - it has no exact scalar
           factor under non-uniform scale/shear, so it is not transformed by the World
           toggle. Counts are space-independent. */
        area   = e->area;
        npntsd = (double)e->npnts;
        npolsd = (double)e->npols;
    } else {
        /* no mesh data: the object-space bounds() box, so the box outputs still work */
        LWDVector bmn = {0,0,0}, bmx = {0,0,0};
        if (od_obj && target) od_obj->bounds(target, bmn, bmx);
        for (k = 0; k < 3; k++) { mn[k] = bmn[k]; mx[k] = bmx[k]; com[k] = 0.5 * (mn[k] + mx[k]); }
        meshvol = area = npntsd = npolsd = 0.0;
    }

    for (k = 0; k < 3; k++) {
        size[k]   = mx[k] - mn[k];
        center[k] = 0.5 * (mn[k] + mx[k]);
    }
    boxvol = size[0] * size[1] * size[2];
    diag   = sqrt(size[0]*size[0] + size[1]*size[1] + size[2]*size[2]);
    if (sphr < 0.0) {           /* no mesh data: the sphere around the box */
        for (k = 0; k < 3; k++) sphc[k] = center[k];
        sphr = 0.5 * diag;
    }

    if (outID == d->out_posbox) {
        /* the shaded point in the same space as the box: world (sg->P) for a world
           box, else the measured object's own space - sg->oP on the surface's own
           object, the world point taken through the reference's inverse otherwise
           (with no mesh data the box is the object-space bounds() box) */
        if (d->world && e) {
            for (k = 0; k < 3; k++) pt[k] = sg->P[k];
        } else if (target == sg->itemID) {
            for (k = 0; k < 3; k++) pt[k] = sg->oP[k];
        } else if (e) {
            double w[3] = { sg->P[0], sg->P[1], sg->P[2] };
            od_xform(e->Mi, w, pt);
        } else {
            for (k = 0; k < 3; k++) pt[k] = sg->P[k];
        }
        for (k = 0; k < 3; k++)
            pos[k] = size[k] > 1e-12 ? (pt[k] - mn[k]) / size[k] : 0.5;
        od_out->setValue(val, pos);
        return;
    }

    if (outID == d->out_size)         od_out->setValue(val, size);
    else if (outID == d->out_center)  od_out->setValue(val, center);
    else if (outID == d->out_min)     od_out->setValue(val, mn);
    else if (outID == d->out_max)     od_out->setValue(val, mx);
    else if (outID == d->out_com)     od_out->setValue(val, com);
    else if (outID == d->out_sphc)    od_out->setValue(val, sphc);
    else if (outID == d->out_sphr)    od_out->setValue(val, &sphr);
    else if (outID == d->out_diag)    od_out->setValue(val, &diag);
    else if (outID == d->out_boxvol)  od_out->setValue(val, &boxvol);
    else if (outID == d->out_meshvol) od_out->setValue(val, &meshvol);
    else if (outID == d->out_area)    od_out->setValue(val, &area);
    else if (outID == d->out_npnts)   od_out->setValue(val, &npntsd);
    else if (outID == d->out_npols)   od_out->setValue(val, &npolsd);
    /* face-centre positions (LW viewport axis convention) */
    else if (outID == d->out_top)     { LWDVector p={center[0], mx[1], center[2]}; od_out->setValue(val, p); }
    else if (outID == d->out_bottom)  { LWDVector p={center[0], mn[1], center[2]}; od_out->setValue(val, p); }
    else if (outID == d->out_right)   { LWDVector p={mx[0], center[1], center[2]}; od_out->setValue(val, p); }
    else if (outID == d->out_left)    { LWDVector p={mn[0], center[1], center[2]}; od_out->setValue(val, p); }
    else if (outID == d->out_front)   { LWDVector p={center[0], center[1], mn[2]}; od_out->setValue(val, p); }
    else if (outID == d->out_back)    { LWDVector p={center[0], center[1], mx[2]}; od_out->setValue(val, p); }
    else {
        LWDVector z = {0,0,0};
        od_out->setValue(val, z);
    }
}

/* ---- XPanel ---- */
/* IDs well away from 0x8000: node panels share that range with Layout's own
   controls (colour pickers wrote to the wrong target on another node). */
enum { OC_WORLD = 0x4F440001, OC_REF = 0x4F440002 };

/* The popup callbacks don't get the node instance, so the panel's current node is
   kept here, set from the GET/SET callbacks (one node panel is open at a time). */
static ODData *g_ui = NULL;
static int     g_refIndex = 0;

static int od_objectCount(void)
{
    LWItemID item;
    int n = 0;
    if (!od_item) return 0;
    for (item = od_item->first(LWI_OBJECT, NULL); item; item = od_item->next(item)) n++;
    return n;
}
static LWItemID od_objectAt(int i)
{
    LWItemID item;
    if (!od_item || i < 0) return NULL;
    for (item = od_item->first(LWI_OBJECT, NULL); item; item = od_item->next(item))
        if (i-- == 0) return item;
    return NULL;
}
static int od_refPopCount(void *ud) { (void)ud; return 1 + od_objectCount(); }
static const char *od_refPopName(void *ud, int i)
{
    LWItemID item;
    (void)ud;
    if (i == 0) return "(Surface's Object)";
    item = od_objectAt(i - 1);
    return item && od_item->name(item) ? od_item->name(item) : "";
}

XCALL_(static void *) ODDataGet(LWInstance inst, unsigned int vid)
{
    ODData *d = (ODData *)inst;
    g_ui = d;
    if (vid == OC_WORLD) return &d->world;
    if (vid == OC_REF) {
        LWItemID item;
        int i = 0;
        g_refIndex = 0;
        if (d->refName[0] && od_item)
            for (item = od_item->first(LWI_OBJECT, NULL); item; item = od_item->next(item), i++) {
                const char *n = od_item->name(item);
                if (n && !strcmp(n, d->refName)) { g_refIndex = i + 1; break; }
            }
        return &g_refIndex;
    }
    return NULL;
}
XCALL_(static LWXPRefreshCode) ODDataSet(LWInstance inst, unsigned int vid, void *value)
{
    ODData *d = (ODData *)inst;
    g_ui = d;
    if (vid == OC_WORLD) d->world = *(int *)value;
    else if (vid == OC_REF) {
        int i = *(int *)value;
        LWItemID item = i > 0 ? od_objectAt(i - 1) : NULL;
        const char *n = item ? od_item->name(item) : NULL;
        if (n) { strncpy(d->refName, n, OD_NAMELEN - 1); d->refName[OD_NAMELEN - 1] = 0; }
        else d->refName[0] = 0;
        d->refID = item;
        d->refTried = 1;
        d->refTriedTime = od_now();
        d->memo_have = 0;
    }
    if (od_iupd) od_iupd(LWNODE_HCLASS, inst);
    return LWXPRC_DRAW;
}
static LWXPanelID od_getPanel(ODData *inst)
{
    static LWXPanelControl ctl[] = {
        { OC_REF,   "Reference Object", "iPopChoice" },
        { OC_WORLD, "World Space",      "iBoolean" },
        { 0 }
    };
    static LWXPanelDataDesc ddesc[] = {
        { OC_REF,   "ref", "integer" },
        { OC_WORLD, "wld", "integer" },
        { 0 }
    };
    static LWXPanelHint hint[] = {
        XpPOPFUNCS(OC_REF, od_refPopCount, od_refPopName),
        XpEND
    };
    LWXPanelID panel;
    if (!od_xpan) return NULL;
    panel = od_xpan->create(LWXP_VIEW, ctl);
    if (panel) {
        od_xpan->describe(panel, ddesc, ODDataGet, ODDataSet);
        od_xpan->hint(panel, 0, hint);
        od_xpan->viewInst(panel, inst);
        od_xpan->setData(panel, 0, inst);
    }
    g_ui = inst;
    return panel;
}

XCALL_(int) ObjectDimsHandler(int version, GlobalFunc *global, void *vlocal, void *serverData)
{
    LWNodeHandler *local = (LWNodeHandler *)vlocal;
    (void)serverData;
    if (version != LWNODECLASS_VERSION) return AFUNC_BADVERSION;
    od_out  = (LWNodeOutputFuncs *)global(LWNODEOUTPUTFUNCS_GLOBAL, GFUSE_TRANSIENT);
    od_in   = (LWNodeInputFuncs  *)global(LWNODEINPUTFUNCS_GLOBAL,  GFUSE_TRANSIENT);
    od_obj  = (LWObjectInfo *)global(LWOBJECTINFO_GLOBAL, GFUSE_TRANSIENT);
    od_item = (LWItemInfo   *)global(LWITEMINFO_GLOBAL,   GFUSE_TRANSIENT);
    od_time = (LWTimeInfo   *)global(LWTIMEINFO_GLOBAL,   GFUSE_TRANSIENT);
    if (!od_out || !od_in) return AFUNC_BADGLOBAL;
    local->inst->create  = ODCreate;
    local->inst->destroy = ODDestroy;
    local->inst->copy    = ODCopy;
    local->inst->load    = ODLoad;
    local->inst->save    = ODSave;
    local->inst->descln  = ODDescribe;
    if (local->rend) {
        local->rend->init    = ODRendInit;
        local->rend->cleanup = ODRendCleanup;
        local->rend->newTime = ODNewTime;
    }
    local->evaluate      = ODEvaluate;
    return AFUNC_OK;
}
XCALL_(int) ObjectDimsInterface(int version, GlobalFunc *global, void *vlocal, void *serverData)
{
    LWInterface *local = (LWInterface *)vlocal;
    (void)serverData;
    if (version != LWINTERFACE_VERSION) return AFUNC_BADVERSION;
    od_xpan = (LWXPanelFuncs *)global(LWXPANELFUNCS_GLOBAL, GFUSE_TRANSIENT);
    od_iupd = (LWInstUpdate  *)global(LWINSTUPDATE_GLOBAL,  GFUSE_TRANSIENT);
    local->panel   = od_getPanel((ODData *)local->inst);
    local->options = NULL;
    local->command = NULL;
    return local->panel ? AFUNC_OK : AFUNC_BADGLOBAL;
}
