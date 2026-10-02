#include "tree3d.h"

// 3D trees (experiment E3 of docs/rendering/remake-research.md). The trees of the forest are terrain records whose
// group 0 draws two or three crossed vertical cards with a painted tree. Each of those records gets a solid core inside
// its cards: a surface of revolution that follows the painted silhouette tier by tier, textured with the same texture
// window, plus a trunk (generated offline from the ROM by tools/upscale/proto/tree3d_gen.py, see tree3d_area3.c).
//
// The core is hooked into the record's group entry as its second display list: func_8007E8D4 emits G_DL to
// (file base + offset), so the entry's dl2 becomes the offset of a small wrapper that calls the original dl2 (if any)
// and then the core. The wrapper only uses G_DL and G_ENDDL, the only commands besides vertices and triangles that the
// CPU walkers of the fade modes (func_8007D798 and others) understand; they copy G_DL without following it. The core
// runs inside the cell's task, after its matrix and with its TERRAIN(x, z) matrix group, so RT64 interpolates it and
// draws it in each VR eye like the cards.
//
// The terrain file is a new copy on every area load, so the entries are checked every frame: one that still has its
// original display lists gets patched again (with the wrapper rebuilt for the new file base). With the option off, the
// patched entries get their original dl2 back.

extern const Tree3DRecord gTree3DArea3[];
extern const int gTree3DArea3Count;

#define TREE3D_MAX_RECORDS 64

// Terrain file (type 0x11) of the loaded area and the area itself.
#define TERRAIN_FILE_BASE (*(volatile u32*)0x800BD9A0)
#define LOADED_AREA (*(volatile s16*)0x801BC450)

static Gfx sTree3DWrappers[TREE3D_MAX_RECORDS][3];

void tree3d_update(void) {
    const Tree3DRecord* table = NULL;
    s32 count = 0;
    s32 enabled;
    u32 base;
    s32 i;

    switch (LOADED_AREA) {
        case 3:
            table = gTree3DArea3;
            count = gTree3DArea3Count;
            break;
        default:
            return;
    }

    base = TERRAIN_FILE_BASE;
    if ((base & 0xFF000000) != 0x80000000) {
        return;
    }

    if (count > TREE3D_MAX_RECORDS) {
        count = TREE3D_MAX_RECORDS;
    }

    enabled = recomp_get_3d_trees_enabled();
    for (i = 0; i < count; i++) {
        const Tree3DRecord* record = &table[i];
        volatile u32* entry = (volatile u32*)(base + record->groupOffset);
        Gfx* wrapper = sTree3DWrappers[i];
        u32 wrapperOffset = (u32)wrapper - base;
        Gfx* g = wrapper;

        // Not the expected file: leave it alone.
        if (entry[1] != record->dl1) {
            continue;
        }

        if (!enabled) {
            if (entry[2] == wrapperOffset) {
                entry[2] = record->dl2;
            }

            continue;
        }

        // Already patched.
        if (entry[2] != record->dl2) {
            continue;
        }

        if (record->dl2 != 0) {
            gSPDisplayList(g++, base + record->dl2);
        }

        gSPDisplayList(g++, record->dl);
        gSPEndDisplayList(g++);
        entry[2] = wrapperOffset;
    }
}
