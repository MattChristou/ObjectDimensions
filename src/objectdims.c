/*
 * objectdims.c - "Object Dimensions" shader node.
 *
 * Reports the bounding box and volume of the object the node's surface is on,
 * as node outputs you can wire into any other input (scale, tiling, displacement
 * amplitude, falloff distance, etc.). Think of it as the missing "size" pins on
 * the Object Info node.
 *
 *   inputs : none (operates on the surface's own object, sg->itemID)
 *   outputs: Size, Center, Min, Max   (vectors, in the selected space)
 *            Top, Bottom, Left, Right, Front, Back  (face-centre positions)
 *            Diagonal, Box Volume, Mesh Volume  (scalars)
 *
 * The six side outputs are the centre point of each bounding-box face, following
 * LightWave's viewport axis convention: Top = +Y, Bottom = -Y, Right = +X,
 * Left = -X, Front = -Z (the face the Front viewport looks at), Back = +Z.
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
 * newTime (where meshInfo works), compute each object's local box + signed-tetra
 * volume, and stash it in a small file-static table keyed by itemID. Evaluate just
 * looks up sg->itemID - a plain read of a table that is only written single-threaded.
 *
 * Coordinate spaces:
 *   - LWObjectInfo->bounds() already returns a WORLD axis-aligned box (confirmed by
 *     the c_inertia SDK sample), so World-space box outputs come straight from it.
 *   - The cached scan uses object-space vertex positions, so Object-space box outputs
 *     and the base volume come from the cache. World mesh volume = object volume
 *     scaled by |det| of the item's linear transform.
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

static LWNodeOutputFuncs *od_out;
static LWNodeInputFuncs  *od_in;
static LWObjectInfo      *od_obj;
static LWItemInfo        *od_item;
static LWXPanelFuncs     *od_xpan;
static LWInstUpdate      *od_iupd;

/* ---- per-object volume/box cache (built single-threaded in newTime/init) ---- */

typedef struct {
    LWItemID item;
    double   mn[3], mx[3];   /* object-space bounding box */
    double   vol;            /* mesh volume in object units^3 */
    double   area;           /* object-space surface area in object units^2 */
    int      npnts, npols;   /* raw mesh point / polygon counts */
} VolEntry;

static VolEntry *g_cache = NULL;
static int       g_count = 0;
static int       g_cap   = 0;
static LWTime    g_time  = 0.0;
static int       g_haveTime = 0;

typedef struct st_ODData {
    NodeID        nodeID;
    NodeOutputID  out_size, out_center, out_min, out_max;
    NodeOutputID  out_top, out_bottom, out_left, out_right, out_front, out_back;
    NodeOutputID  out_diag, out_boxvol, out_meshvol;
    NodeOutputID  out_area, out_npnts, out_npols;
    int           world;      /* 0 = object/local space, 1 = world space */

    /* Per-instance memo of a lazy mesh scan. When the render-time cache (g_cache)
       is cold - e.g. this node drives a ProGeo graph rather than shading a surface,
       so rend->newTime/init never populated it - Evaluate would otherwise re-scan
       the entire mesh via od_computeForItem on every single sample. sg->itemID is
       constant for a given node instance, so we scan once and cache the result here.
       (Concurrent first-time writes across render threads are benign: every thread
       computes identical values.) */
    LWItemID      memo_item;
    double        memo_mn[3], memo_mx[3];
    double        memo_vol, memo_area;
    int           memo_npnts, memo_npols;
    int           memo_have;  /* 0 = not scanned, 1 = scanned OK, 2 = scanned empty */
} ODData;

#define ID_WLD LWID_('W','L','D',' ')
static LWBlockIdent odblk[] = { {ID_WLD,"wld"}, {0} };

/* ---- mesh scan: object-space bbox + signed-tetra volume ---- */

typedef struct {
    LWMeshInfoID mesh;
    double mn[3], mx[3];
    double vol;
    double area;
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
        double a[3], b[3], cx, cy, cz, ex, ey, ez, fx, fy, fz, tx, ty, tz;
        c->mesh->pntOtherPos(c->mesh, c->mesh->polVertex(c->mesh, pol, i), fp);
        a[0] = fp[0]; a[1] = fp[1]; a[2] = fp[2];
        c->mesh->pntOtherPos(c->mesh, c->mesh->polVertex(c->mesh, pol, i + 1), fp);
        b[0] = fp[0]; b[1] = fp[1]; b[2] = fp[2];
        /* signed volume of tetra (origin, v0, a, b) = v0 . (a x b) / 6 */
        cx = a[1]*b[2] - a[2]*b[1];
        cy = a[2]*b[0] - a[0]*b[2];
        cz = a[0]*b[1] - a[1]*b[0];
        c->vol += (v0[0]*cx + v0[1]*cy + v0[2]*cz) / 6.0;
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

/* Compute object-space bbox + mesh volume + surface area + point/poly counts for
   one item. Returns 1 on success. Count/area out-params may be NULL. */
static int od_computeForItem(LWItemID item, double mn[3], double mx[3], double *vol,
                             double *area, int *npnts, int *npols)
{
    LWMeshInfoID mesh;
    ScanCtx ctx;
    int k, np = 0, ng = 0;

    if (!od_obj || !item) return 0;
    mesh = od_obj->meshInfo(item, 0);
    if (!mesh) return 0;

    ctx.mesh = mesh;
    ctx.vol  = 0.0;
    ctx.area = 0.0;
    for (k = 0; k < 3; k++) { ctx.mn[k] = 1e30; ctx.mx[k] = -1e30; }
    mesh->scanPolys(mesh, (LWPolScanFunc *)od_polyScan, &ctx);
    if (mesh->numPoints)   np = mesh->numPoints(mesh);
    if (mesh->numPolygons) ng = mesh->numPolygons(mesh);
    if (mesh->destroy) mesh->destroy(mesh);

    if (ctx.mx[0] < ctx.mn[0]) return 0;   /* nothing scanned */
    for (k = 0; k < 3; k++) { mn[k] = ctx.mn[k]; mx[k] = ctx.mx[k]; }
    *vol = fabs(ctx.vol);
    if (area)  *area  = ctx.area;
    if (npnts) *npnts = np;
    if (npols) *npols = ng;
    return 1;
}

/* Rebuild the whole-scene cache. Single-threaded (called from init/newTime). */
static void od_rebuildCache(void)
{
    LWItemID item;
    int k;

    g_count = 0;
    if (!od_item || !od_obj) return;

    for (item = od_item->first(LWI_OBJECT, NULL); item; item = od_item->next(item)) {
        double mn[3], mx[3], vol, area = 0.0;
        int npnts = 0, npols = 0;
        if (!od_computeForItem(item, mn, mx, &vol, &area, &npnts, &npols)) continue;
        if (g_count >= g_cap) {
            int ncap = g_cap ? g_cap * 2 : 16;
            VolEntry *n = (VolEntry *)realloc(g_cache, ncap * sizeof(VolEntry));
            if (!n) break;
            g_cache = n; g_cap = ncap;
        }
        g_cache[g_count].item = item;
        for (k = 0; k < 3; k++) { g_cache[g_count].mn[k] = mn[k]; g_cache[g_count].mx[k] = mx[k]; }
        g_cache[g_count].vol   = vol;
        g_cache[g_count].area  = area;
        g_cache[g_count].npnts = npnts;
        g_cache[g_count].npols = npols;
        g_count++;
    }
}

static VolEntry *od_lookup(LWItemID item)
{
    int i;
    for (i = 0; i < g_count; i++)
        if (g_cache[i].item == item) return &g_cache[i];
    return NULL;
}

/* |det| of the 3x3 linear part of a 4x4 (magnitude is layout-independent). */
static double od_linearDet(const LWDMatrix4 M)
{
    double d =
        M[0][0]*(M[1][1]*M[2][2] - M[1][2]*M[2][1])
      - M[0][1]*(M[1][0]*M[2][2] - M[1][2]*M[2][0])
      + M[0][2]*(M[1][0]*M[2][1] - M[1][1]*M[2][0]);
    return fabs(d);
}

/* ---- render lifecycle (single-threaded; meshInfo is valid here) ---- */

XCALL_(static LWError) ODRendInit(LWInstance inst, int mode)
{
    (void)inst; (void)mode;
    od_rebuildCache();
    g_haveTime = 0;          /* let the first newTime refresh for the frame's pose */
    return NULL;
}
XCALL_(static void) ODRendCleanup(LWInstance inst)
{
    (void)inst;
    g_haveTime = 0;          /* keep the buffer; force a rebuild next render */
}
XCALL_(static LWError) ODNewTime(LWInstance inst, LWFrame frame, LWTime t)
{
    (void)inst; (void)frame;
    if (!g_haveTime || t != g_time) {
        od_rebuildCache();
        g_time = t;
        g_haveTime = 1;
    }
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
    NodeID id = da->nodeID;
    NodeOutputID s=da->out_size, c=da->out_center, mn=da->out_min, mx=da->out_max,
                 dg=da->out_diag, bv=da->out_boxvol, mv=da->out_meshvol,
                 tp=da->out_top, bt=da->out_bottom, lf=da->out_left,
                 rt=da->out_right, fr=da->out_front, bk=da->out_back,
                 ar=da->out_area, pc=da->out_npnts, gc=da->out_npols;
    *da = *db;
    da->nodeID=id; da->out_size=s; da->out_center=c; da->out_min=mn; da->out_max=mx;
    da->out_diag=dg; da->out_boxvol=bv; da->out_meshvol=mv;
    da->out_top=tp; da->out_bottom=bt; da->out_left=lf;
    da->out_right=rt; da->out_front=fr; da->out_back=bk;
    da->out_area=ar; da->out_npnts=pc; da->out_npols=gc;
    da->memo_have = 0;          /* force a fresh scan for this instance's own object */
    return NULL;
}
XCALL_(static LWError) ODLoad(LWInstance inst, const LWLoadState *ls)
{
    ODData *d = (ODData *)inst;
    LWID id;
    while ((id = LWLOAD_FIND(ls, odblk))) {
        if (id == ID_WLD) LWLOAD_I4(ls, &d->world, 1);
        LWLOAD_END(ls);
    }
    return NULL;
}
XCALL_(static LWError) ODSave(LWInstance inst, const LWSaveState *ss)
{
    ODData *d = (ODData *)inst;
    LWSAVE_BEGIN(ss, &odblk[0], 0); LWSAVE_I4(ss, &d->world, 1); LWSAVE_END(ss);
    return NULL;
}
XCALL_(static const char *) ODDescribe(LWInstance inst) { (void)inst; return "Object Dimensions"; }

XCALL_(static void) ODEvaluate(LWInstance inst, LWShadingGeometry *sg, NodeOutputID outID, NodeValue val)
{
    ODData *d = (ODData *)inst;
    double mn[3], mx[3], size[3], center[3], boxvol, meshvol, diag, localVol = 0.0;
    double lmn[3], lmx[3], localArea = 0.0, area, npntsd, npolsd;
    int localPnts = 0, localPols = 0;
    int haveLocal = 0, k;
    VolEntry *e;

    /* object-space box + volume + area + counts: prefer the render-time cache; else a
       per-instance memo; else scan once and memoise (never scan the whole mesh per sample). */
    e = od_lookup(sg->itemID);
    if (e) {
        for (k = 0; k < 3; k++) { lmn[k] = e->mn[k]; lmx[k] = e->mx[k]; }
        localVol = e->vol; localArea = e->area; localPnts = e->npnts; localPols = e->npols;
        haveLocal = 1;
    } else if (d->memo_have && d->memo_item == sg->itemID) {
        if (d->memo_have == 1) {
            for (k = 0; k < 3; k++) { lmn[k] = d->memo_mn[k]; lmx[k] = d->memo_mx[k]; }
            localVol = d->memo_vol; localArea = d->memo_area;
            localPnts = d->memo_npnts; localPols = d->memo_npols;
            haveLocal = 1;
        }
        /* memo_have == 2: known-empty scan (e.g. meshInfo NULL mid-shading) - don't retry */
    } else {
        double smn[3], smx[3], svol = 0.0, sarea = 0.0;
        int spnts = 0, spols = 0;
        if (od_computeForItem(sg->itemID, smn, smx, &svol, &sarea, &spnts, &spols)) {
            for (k = 0; k < 3; k++) {
                lmn[k] = d->memo_mn[k] = smn[k];
                lmx[k] = d->memo_mx[k] = smx[k];
            }
            localVol   = d->memo_vol   = svol;
            localArea  = d->memo_area  = sarea;
            localPnts  = d->memo_npnts = spnts;
            localPols  = d->memo_npols = spols;
            haveLocal = 1;
            d->memo_item = sg->itemID;
            d->memo_have = 1;      /* publish last */
        } else {
            d->memo_item = sg->itemID;
            d->memo_have = 2;      /* mark tried-and-empty so we stop rescanning */
        }
    }

    if (d->world) {
        /* bounds() is already a world-space AABB */
        if (od_obj && sg->itemID) {
            LWDVector bmn, bmx;
            od_obj->bounds(sg->itemID, bmn, bmx);
            for (k = 0; k < 3; k++) { mn[k] = bmn[k]; mx[k] = bmx[k]; }
        } else if (haveLocal) {
            for (k = 0; k < 3; k++) { mn[k] = lmn[k]; mx[k] = lmx[k]; }
        } else {
            for (k = 0; k < 3; k++) { mn[k] = mx[k] = 0.0; }
        }
        meshvol = localVol * od_linearDet(sg->toWorld);
    } else {
        if (haveLocal) {
            for (k = 0; k < 3; k++) { mn[k] = lmn[k]; mx[k] = lmx[k]; }
        } else if (od_obj && sg->itemID) {
            LWDVector bmn, bmx;
            od_obj->bounds(sg->itemID, bmn, bmx);
            for (k = 0; k < 3; k++) { mn[k] = bmn[k]; mx[k] = bmx[k]; }
        } else {
            for (k = 0; k < 3; k++) { mn[k] = mx[k] = 0.0; }
        }
        meshvol = localVol;
    }

    for (k = 0; k < 3; k++) {
        size[k]   = mx[k] - mn[k];
        center[k] = 0.5 * (mn[k] + mx[k]);
    }
    boxvol = size[0] * size[1] * size[2];
    diag   = sqrt(size[0]*size[0] + size[1]*size[1] + size[2]*size[2]);
    /* Surface Area is a modelled (object-space) property - it has no exact scalar
       factor under non-uniform scale/shear, so it is not transformed by the World
       toggle. Counts are space-independent. */
    area   = localArea;
    npntsd = (double)localPnts;
    npolsd = (double)localPols;

    if (outID == d->out_size)         od_out->setValue(val, size);
    else if (outID == d->out_center)  od_out->setValue(val, center);
    else if (outID == d->out_min)     od_out->setValue(val, mn);
    else if (outID == d->out_max)     od_out->setValue(val, mx);
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
enum { OC_WORLD = 0x8001 };

XCALL_(static void *) ODDataGet(LWInstance inst, unsigned int vid)
{
    ODData *d = (ODData *)inst;
    if (vid == OC_WORLD) return &d->world;
    return NULL;
}
XCALL_(static LWXPRefreshCode) ODDataSet(LWInstance inst, unsigned int vid, void *value)
{
    ODData *d = (ODData *)inst;
    if (vid == OC_WORLD) d->world = *(int *)value;
    if (od_iupd) od_iupd(LWNODE_HCLASS, inst);
    return LWXPRC_DRAW;
}
static LWXPanelID od_getPanel(ODData *inst)
{
    static LWXPanelControl ctl[] = {
        { OC_WORLD, "World Space", "iBoolean" },
        { 0 }
    };
    static LWXPanelDataDesc ddesc[] = {
        { OC_WORLD, "wld", "integer" },
        { 0 }
    };
    LWXPanelID panel;
    if (!od_xpan) return NULL;
    panel = od_xpan->create(LWXP_VIEW, ctl);
    if (panel) {
        od_xpan->describe(panel, ddesc, ODDataGet, ODDataSet);
        od_xpan->viewInst(panel, inst);
        od_xpan->setData(panel, 0, inst);
    }
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
