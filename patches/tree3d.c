#include "tree3d.h"

// 3D trees (experiment E3 of docs/rendering/remake-research.md). The trees of the forest are terrain records whose
// group 0 draws two or three crossed vertical cards with a painted tree. Each of those records gets a solid core inside
// its cards: a surface of revolution that follows the painted silhouette tier by tier, textured with the same texture
// window, plus a trunk (generated offline from the ROM by tools/upscale/proto/tree3d_gen.py, see tree3d_area3.c).
//
// The core is hooked into the record's group entry as its second display list: func_8007E8D4 emits G_DL to
// (file base + offset), so the entry's dl2 becomes the offset of a generated list (entryDl): a copy of the original
// second list, if the entry has one, that calls the core at its end. In fade mode 2, func_8007E8D4 doesn't emit dl2
// but hands it to the CPU walker func_8007D798, which drops the quads close to the camera: the copy keeps the card
// quads inline so they're still culled, and the walker copies the call to the core without following it (the core
// isn't culled). The lists only use commands the walkers understand. The core runs inside the cell's task, after its
// matrix and with its TERRAIN(x, z) matrix group, so RT64 interpolates it and draws it in each VR eye like the cards.
//
// The terrain file is a new copy on every area load, so the entries are checked every frame: one that still has its
// original display lists gets patched again. With the option off, the patched entries get their original dl2 back.

extern const Tree3DRecord gTree3DArea3[];
extern const int gTree3DArea3Count;

#define TREE3D_MAX_RECORDS 64

// Terrain file (type 0x11) of the loaded area and the area itself.
#define TERRAIN_FILE_BASE (*(volatile u32*)0x800BD9A0)
#define LOADED_AREA (*(volatile s16*)0x801BC450)

// File base each record was patched for (0 if it isn't), so an entry patched for another base (a file moved with the
// patched offset in it) is recognized and fixed instead of being left pointing somewhere else.
static u32 sTree3DPatchedBase[TREE3D_MAX_RECORDS];

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
        u32 patchedOffset = (u32)record->entryDl - base;
        u32 oldBase = sTree3DPatchedBase[i];
        s32 patched;

        // Not the expected file: leave it alone.
        if (entry[1] != record->dl1) {
            continue;
        }

        // Patched for this base, or for another one the file was moved from.
        patched = (entry[2] == patchedOffset) || ((oldBase != 0) && (entry[2] == (u32)record->entryDl - oldBase));
        if (!enabled) {
            if (patched) {
                entry[2] = record->dl2;
                sTree3DPatchedBase[i] = 0;
            }

            continue;
        }

        if (patched || (entry[2] == record->dl2)) {
            entry[2] = patchedOffset;
            sTree3DPatchedBase[i] = base;
        }
    }
}
