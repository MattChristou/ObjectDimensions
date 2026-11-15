OBJECT DIMENSIONS for LightWave 3D  -  version 1.0.0-beta.1
==========================================================

Thanks for testing! Object Dimensions is a node that tells the rest of your
node graph how big an object is: its bounding box, size, centre, volume,
surface area and more. Wire those into texture scales, gradients, falloffs and
displacement amounts, and they fit the object automatically - no retuning when
the model changes size.

This is a BETA. Please don't use it on production work you can't afford to
redo, and please tell us about anything odd (see "Reporting problems" below).


REQUIREMENTS
------------
- LightWave 3D 2025.
- Windows 64-bit, or macOS 11 or later (Apple Silicon or Intel).
- LightWave 2026 is untested; reports are welcome.


INSTALLING
----------
Windows
1. Copy  Plugins\Windows\ObjectDimensions.p  to any folder you keep plug-ins in.
2. In Layout: Utilities tab > Plugins > Add Plugins..., and pick
   ObjectDimensions.p.

macOS
1. Double-click  Plugins/macOS/ObjectDimensions-mac.zip  to unpack
   ObjectDimensions.plugin, and move that to any folder you keep plug-ins in.
   (It is zipped separately so it arrives exactly as it was built on a Mac.)
2. The download is not signed by Apple yet, so macOS may block it. In
   Terminal, run (with your own path):
       xattr -dr com.apple.quarantine /path/to/ObjectDimensions.plugin
3. In Layout: Utilities tab > Plugins > Add Plugins..., and pick
   ObjectDimensions.plugin.

The node then appears in every Node Editor under Add Node > Tools >
Object Dimensions.

To update to a newer beta later, quit LightWave, replace the plug-in, and
restart.


QUICK START (two minutes)
-------------------------
1. Make any object (a box or a ball is fine) and load it into Layout.
2. Open its surface in the Surface Editor and click Edit Nodes.
3. Add Node > Tools > Object Dimensions.
4. Wire its "Position in Box" output into the Color input of the material
   (e.g. Principled BSDF). Render or turn on VPR.
   You should see a colour cube stretched over the object: red grows from
   left to right, green from bottom to top, blue from front to back, whatever
   the object's size.
5. Move and rotate the object. With World Space on (the default) the cube
   stays aligned to the world; turn World Space off and it sticks to the
   object.


OUTPUTS
-------
Size, Center, Min, Max          the bounding box
Top, Bottom, Left, Right,
Front, Back                     the centre of each side of the box
                                (Top = +Y, Right = +X, Front = -Z, as in
                                LightWave's viewports)
Position in Box                 where the point being shaded sits in the box,
                                0 to 1 on each axis - feed it to gradients
Center of Mass                  the volume centre of a closed mesh (the box
                                centre for open meshes)
Sphere Center, Sphere Radius    the smallest sphere around the box centre that
                                holds every point - for spherical falloffs
Diagonal                        length of the box's diagonal
Box Volume                      width x height x depth
Mesh Volume                     the true volume of a closed mesh
Surface Area                    total polygon area (always in object units)
Point Count, Polygon Count      the mesh's counts


CONTROLS
--------
Reference Object
    "(Surface's Object)" measures the object the surface is on. Pick another
    object to measure that one instead - for example to size a texture by a
    character's height, or to use the node in a light, camera or environment
    node graph, where there is no surface object of its own.
World Space
    On: measurements in world units after the object's move, rotation and
    scale (the box is the object's footprint on the world grid).
    Off: the object's own modelled dimensions, ignoring how it is placed.


KNOWN LIMITATIONS
-----------------
- Deforming objects (bones, morphs, displacement) are measured in their rest
  shape.
- Mesh Volume and Center of Mass only make sense for closed meshes.
- With World Space on and an object scaled unevenly (e.g. stretched on one
  axis), Sphere Radius is slightly larger than it needs to be - never
  smaller.
- In environment, camera and light graphs, pick a Reference Object, or every
  output is zero.


WHAT WE'D MOST LIKE YOU TO TEST
-------------------------------
- The quick start above, with World Space on and off, on a moved and rotated
  object.
- Wiring outputs into texture scale, gradients and falloffs on your own
  objects.
- The Reference Object list (does it show your scene's objects, and does the
  choice survive saving and reloading the scene?).
- The node in the displacement Node Editor (Node Displacement), on the object
  being displaced. Please tell us if anything there is slow, wrong or crashes.
- That results match between VPR, F9 and F10 / network renders.
- On a Mac: that the plug-in loads at all, and the quick start.


REPORTING PROBLEMS
------------------
Please include:
- the version (1.0.0-beta.1) and your LightWave version and platform;
- which output you used, the Reference Object and World Space settings
  (a screenshot of the node graph is ideal);
- what you expected and what happened instead, and the steps to reproduce it;
- if you can, the scene and object (or a cut-down version that still shows
  the problem).

Send reports to: matthew.christou@gmail.com
