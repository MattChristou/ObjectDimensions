# ObjectDimensions — Changelog

## Unreleased

### Added
- **`Position in Box` output** (vector): where the shaded point sits inside the
  measured box, 0 to 1 per axis (0.5 on a flat axis), in the same space as the box.
  Wire it into a gradient for a bottom-to-top fade or an edge falloff that fits any
  object without retuning.
- **`Center of Mass` output** (vector): the volume centroid of a closed mesh, from the
  same scan as Mesh Volume (signed sums, so either winding works). Falls back to the
  box centre when the mesh encloses no volume. Follows the World Space toggle.
- **Reference Object** picker: measure a chosen object instead of the surface's own
  ("(Surface's Object)", the default). Saved by name; if the object is missing, the
  node measures its own object. In object space, Position in Box takes the shaded
  point into the reference's own space.
- **macOS build.** `build_mac.sh` makes a universal (Apple Silicon + Intel)
  `ObjectDimensions.plugin`, ad-hoc signed, and checks that it loads and registers
  the node on both chip types. GitHub Actions builds Windows and macOS on every push.
- **Plugin info record** (`lwf_plugin_info`, from `lwf_version.h`): id
  `ObjectDimensions`, version 1.0.0, so the Plugin Updates checker can compare an
  installed copy with the catalogue. CI checks the export on both platforms.

### Fixed (2026-11-15)
- **World Space showed the object-space box.** `LWObjectInfo->bounds()` returns the
  box in object space (confirmed in Layout 2026-10-31), not a world AABB as assumed,
  so moving or rotating the object didn't change the World outputs. World values now
  come from the item's `LWIP_TRANSFORM` (read single-threaded in the render setup):
  the 8 local box corners transformed and re-boxed, and Mesh Volume scaled by its
  |det|. `LWShadingGeometry.toWorld` is no longer used (it has no translation).
- **Stale numbers after editing the mesh.** The per-node fallback scan (used when the
  render-time cache is cold, e.g. in ProGeo graphs) was kept forever; it is now
  dropped on render start, on each new frame and whenever the time changes.
- **World Space control ID** moved from 0x8001 to 0x4F440001: IDs just above 0x8000
  on node panels collide with Layout's own controls.
- Header comment in `objectdims.c` brought up to date with the outputs.

### Changed
- LightWave 2025 SDK headers are vendored in `sdk/include`; the project uses them
  instead of `C:\Lightwave_SDK\include`.

### Fixed
- **Severe slowdown when the render-time cache is cold (e.g. driving a ProGeo
  graph).** `Evaluate`'s fallback scanned the *entire* mesh via `meshInfo()` +
  `scanPolys` on **every sample** whenever `od_lookup` missed — which is always the
  case outside the surface-render lifecycle, since only `rend->newTime`/`init`
  populate the shared cache. On a 400k-poly object that meant a full-mesh scan per
  evaluation (hours of render time). The lazy scan is now memoised per node instance
  (`sg->itemID` is constant for a given instance), so an object is scanned at most
  once instead of once per sample. A failed/empty scan is also remembered so it is
  not retried every sample.

### Added
- **`Surface Area` output** (scalar) — total mesh surface area, summed as
  `½|(a−v0)×(b−v0)|` over the same triangle fan already walked for Mesh Volume, so
  it costs nothing extra. This is a modelled **object-space** property: unlike Mesh
  Volume (which scales exactly by `|det|`), area has no exact scalar factor under
  non-uniform scale/shear, so it is **not** transformed by the World Space toggle.
- **`Point Count` / `Polygon Count` outputs** (scalars) — raw mesh counts from
  `meshInfo->numPoints`/`numPolygons`. Space-independent; handy for driving
  detail/LOD or density logic.
- **Six face-centre outputs** — `Top`, `Bottom`, `Left`, `Right`, `Front`, `Back`
  (vectors): the centre point of each bounding-box face, so you can wire the position
  of any side straight into a placement/position input. Honour the Local/World toggle.
  Axis convention follows LightWave's viewports: Top=+Y, Bottom=−Y, Right=+X, Left=−X,
  Front=−Z (the face the Front viewport looks at), Back=+Z.

### Fixed
- **Mesh Volume always 0.** The volume/local-box scan used `LWObjectInfo->meshInfo()`
  lazily inside the surface shader, but that global returns NULL during multithreaded
  shading — so the scan never ran. Moved the whole-scene scan into the render-lifecycle
  callbacks (`newTime`/`init`), where `meshInfo()` is valid, and cache each object's
  object-space box + volume in a file-static table keyed by itemID. Evaluate now just
  looks up `sg->itemID`. Also switched the scan to `pntOtherPos` + `LWPOLTYPE_FACE`
  filtering (matching the c_inertia SDK sample) and take World-space box outputs
  straight from `bounds()` (which is already a world AABB).

## Build 1 (2026-07-09)

Initial release. One node, group **Tools**:

### Object Dimensions
Reports the bounding box and volume of the object the node's surface is on, as
outputs you can wire into any other node input (scale, tiling, displacement
amplitude, falloff distance, etc.). The "size" pins the Object Info node never had.

**Outputs**
- `Size` (vector) — bounding-box extents (W, H, D) in the selected space
- `Center` (vector) — box midpoint
- `Min` / `Max` (vectors) — box corners
- `Diagonal` (scalar) — length of `Size`
- `Box Volume` (scalar) — `Sx · Sy · Sz`
- `Mesh Volume` (scalar) — true volume via signed-tetrahedra sum over polygons
  (approximate for non-closed meshes)

**World Space** toggle (default on)
- off → object/local space: modelled rest dimensions in object units
- on  → world space: axis-aligned bounding box after the item's transform
  (position/rotation/scale), i.e. its footprint on the world grid. A rotated
  object reports the enlarged enclosing AABB.

**Implementation**
- Local bbox + mesh volume computed once per object by scanning base (rest) vertex
  positions, cached on the instance by `itemID`. `LWObjectInfo->bounds()` seeds a
  provisional box for frame 0.
- World outputs transform the 8 local corners by `sg->toWorld`; the matrix's
  row/column convention is auto-calibrated at runtime against the hit's own
  (`oP` → `P`) pair. World mesh volume scales by `|det|` of the transform.
- Context: Surface (`NCON_SURF`); operates on `sg->itemID` (the surface's own object).

**Known limitations / possible follow-ups**
- Self-object only. A picked "reference item" mode (report another object's size)
  is not implemented yet.
- `Mesh Volume` assumes a closed, consistently-wound mesh.
- Deforming meshes report rest-pose dimensions (base positions). A deformed/current
  mode (`pntOtherPos`) could be added.
