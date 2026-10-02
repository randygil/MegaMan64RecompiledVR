#ifndef __TREE3D_H__
#define __TREE3D_H__

#include "patch_helpers.h"

// Whether the player wants the 3D cores (graphics option "Trees").
DECLARE_FUNC(s32, recomp_get_3d_trees_enabled);

#ifdef MIPS
#include "patches.h"

// A tree record of an area's terrain file (type 0x11) that gets a 3D core: the group entry that draws its crossed
// cards (offset in the file), the display lists the entry is expected to have, and the display list that replaces the
// second one (a copy of the original second list, if any, that calls the core at its end).
typedef struct {
    s32 record;
    u32 groupOffset;
    u32 dl1;
    u32 dl2;
    Gfx* entryDl;
} Tree3DRecord;

// Called once per frame before the scene is drawn: adds the 3D cores to the trees of the loaded terrain, or takes them
// away when the option is off.
void tree3d_update(void);
#endif

#endif
