/*
 * od_module.c - ObjectDimensions plugin entry point.
 *
 * Registers one standalone utility node (group "Tools"):
 *   - "Object Dimensions" (objectdims.c) - bounding box + volume of the surface's
 *     own object, exposed as outputs (Size / Center / Min / Max / Diagonal /
 *     Box Volume / Mesh Volume) for wiring into other node inputs.
 */

#include <lwserver.h>
#include <lwmodule.h>
#include <lwnodes.h>
#include "lwf_version.h"

extern int ObjectDimsHandler(int, GlobalFunc *, void *, void *);
extern int ObjectDimsInterface(int, GlobalFunc *, void *, void *);

static ServerTagInfo ObjectDimsTags[] = {
    { NCON_ALL, SRVTAG_NODECONTEXT },
    { "Tools", SRVTAG_NODEGROUP | LANGID_USENGLISH },
    { "Object Dimensions", SRVTAG_USERNAME | LANGID_USENGLISH },
    { NULL, 0 }
};

static ServerRecord ServerDesc[] = {
    { LWNODE_HCLASS, "AP_ObjectDims", (ActivateFunc *)ObjectDimsHandler,   ObjectDimsTags },
    { LWNODE_ICLASS, "AP_ObjectDims", (ActivateFunc *)ObjectDimsInterface, ObjectDimsTags },
    { NULL }
};

void *Startup(void) { return (void *)1; }
void Shutdown(void *serverData) { (void)serverData; }

#ifdef _WIN32
__declspec(dllexport)
#endif
ModuleDescriptor _mod_descrip = {
    MOD_SYSSYNC, MOD_SYSVER, MOD_MACHINE, Startup, Shutdown, ServerDesc
};

/* This .p's id and version, for the update checker (lwf_version.h). */
LWF_PLUGIN_INFO("ObjectDimensions", "1.0.0");
