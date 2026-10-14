#include "common.h"

#include <cstring>
#include <memory>
#include <utility>

#include "dataread.hpp"
#include "editanalyze.hpp"
#include "editmap.hpp"
#include "mainloop.hpp"
#include "mapjump.hpp"
#include "mapselect.hpp"
#include "mg_memory.hpp"
#include "mg_texture.hpp"
#include "mglib.hpp"
#include "platform/config.hpp"
#include "sceneload.hpp"
#include "town_seamless.hpp"

extern MapJumpMapInfo PortSubMapInfo asm("MapJump_SubMapInfo");
extern int            PortNowSubMapNo asm("MapJump_NowSubMapNo");
extern int            PortSubMapLoadBG asm("EditLoop_SubMapLoadBG");
uptr                  PortSceneLoadMapData(SCN_LOADMAP_INFO2 &info, int deferred) asm("SceneLoad_LoadMapData");

namespace {

constexpr int kSubMapSlot = 1;
constexpr int kSeamlessSlot = 2;
// Sub map numbers of m04 and the lakeside m05, the two whose swap retail hides.
constexpr int kLakeNeighbour = 13;
constexpr int kLake = 14;
// Room for one sub map; the PS2 had 32 MiB in all.
constexpr int kSeamlessStackQuadwords = 64 * 1024 * 1024 / 16;

std::unique_ptr<u_long128[]> SeamlessStorage;
mgCMemory                    SeamlessStack;
// The scene that holds the map in slot 2, and that map's sub map number; -1 when there is none.
CScene                      *SeamlessScene = nullptr;
int                          SeamlessMapNo = -1;
// Slot 1 holds the map that was swapped in from slot 2, so it uses SeamlessStack and the added texture blocks,
// and slot 2 uses the retail sub map stack and blocks.
bool                         SlotOneSeamless = false;
// A load that failed is not retried until a sub map is loaded the retail way.
bool                         SeamlessFailed = false;

int Partner(int sub_map_no) {
    if (sub_map_no == kLakeNeighbour) {
        return kLake;
    }
    if (sub_map_no == kLake) {
        return kLakeNeighbour;
    }
    return -1;
}

bool SlotHolds(CScene *scene, int slot, int sub_map_no) {
    CSceneMap  *data = scene->GetSceneMap(slot);
    const char *name = GetMapName(sub_map_no, NULL);
    return data != NULL && data->status != 0 && name != NULL && std::strcmp(data->name, name) == 0;
}

bool InPalmBrinks(CScene *scene) {
    CSceneMap *data = scene->GetSceneMap(0);
    return data != NULL && data->status != 0 && std::strcmp(data->name, "m01") == 0;
}

bool TableHasRoom() {
    return mgTexManager.block_max >= kTownTextureBlocks + kSeamlessTextureBlocks;
}

// Forgets slot 2 when the scene emptied it itself.
void Revalidate(CScene *scene) {
    if (SeamlessScene != scene) {
        return;
    }
    if (SeamlessMapNo >= 0 && !SlotHolds(scene, kSeamlessSlot, SeamlessMapNo)) {
        SeamlessMapNo = -1;
    }
    CSceneMap *slot = scene->GetSceneMap(kSubMapSlot);
    if (SlotOneSeamless && (slot == NULL || slot->status == 0 || slot->stack != &SeamlessStack)) {
        SlotOneSeamless = false;
    }
}

void DropSeamless(CScene *scene) {
    Revalidate(scene);
    if (SeamlessScene != scene || SeamlessMapNo < 0) {
        return;
    }
    scene->DeleteMap(kSeamlessSlot, 0);
    SeamlessMapNo = -1;
}

// Loads sub_map_no into slot 2 in SeamlessStack, as LoadSubMap loads slot 1.
bool LoadSeamless(CScene *scene, int sub_map_no) {
    char *map_name = GetMapName(sub_map_no, NULL);
    if (map_name == NULL) {
        return false;
    }
    if (!SeamlessStorage) {
        SeamlessStorage = std::make_unique<u_long128[]>(kSeamlessStackQuadwords);
        std::strcpy(SeamlessStack.name, "Seamless lake");
    }
    SeamlessStack.stSetBuffer(SeamlessStorage.get(), kSeamlessStackQuadwords);
    SeamlessStack.stack_used = 0;
    SeamlessStack.lock = 0;

    SCN_LOADMAP_INFO2 info;
    char              map_path[0x40];
    char              file_name[0x20];
    info.tex_block = kTownTextureBlocks;
    info.efp_tex_block = kTownTextureBlocks + kSeamlessTextureBlocks - 1;
    info.load_buf = PortSubMapInfo.load_buf;
    info.place_parts_max = 0x140;
    GetMapPath(map_path, map_name);
    DivPathName(map_path, info.files[0].dir, file_name);
    info.files[0].enable = 1;
    std::strcpy(info.files[0].map_name, file_name);
    std::strcpy(info.files[0].cfg_name, file_name);
    std::strcpy(info.files[0].mpk_name, file_name);
    std::strcpy(info.files[0].ipk_name, file_name);
    std::strcpy(info.files[0].efp_name, file_name);
    std::strcpy(info.name, file_name);
    info.stack = &SeamlessStack;
    info.data_ready = 1;
    info.map_no = kSeamlessSlot;

    if (PortSceneLoadMapData(info, 0) == 0 || scene->LoadMapFromMemory(kSeamlessSlot, &info) < 0) {
        if (scene->GetMap(kSeamlessSlot) != NULL) {
            scene->DeleteMap(kSeamlessSlot, 0);
        }
        return false;
    }

    scene->SetActive(SCENE_DATA_MAP, kSeamlessSlot);
    EditMapInitEvent(sub_map_no, (CEditMap *) scene->GetMap(kSeamlessSlot));
    SeamlessScene = scene;
    SeamlessMapNo = sub_map_no;
    return true;
}

// Brings the map in slot 2 into slot 1 and the one in slot 1 into slot 2, with their stacks and textures.
bool SwapIn(CScene *scene, int sub_map_no) {
    Revalidate(scene);
    if (!ConfigGet().seamless_lake || SeamlessScene != scene || SeamlessMapNo != sub_map_no ||
        PortNowSubMapNo != Partner(sub_map_no) || !SlotHolds(scene, kSubMapSlot, PortNowSubMapNo) ||
        !SlotHolds(scene, kSeamlessSlot, sub_map_no)) {
        return false;
    }

    scene->DeleteSubVillager();
    std::swap(*scene->GetSceneMap(kSubMapSlot), *scene->GetSceneMap(kSeamlessSlot));
    scene->SetActive(SCENE_DATA_MAP, kSubMapSlot);
    scene->SetActive(SCENE_DATA_MAP, kSeamlessSlot);
    SlotOneSeamless = !SlotOneSeamless;
    SeamlessMapNo = PortNowSubMapNo;

    // As LoadSubMap records a loaded sub map.
    s16 *sub_map_nos = &GetSaveData()->map_no;
    sub_map_nos[3] = PortNowSubMapNo;
    scene->SetNowSubMapNo(sub_map_no);
    PortNowSubMapNo = sub_map_no;
    sub_map_nos[1] = sub_map_no;
    return true;
}

} // namespace

PORT_OVERRIDE int LoadSubMap(CScene *scene, int sub_map_no, int flag) {
    if (SwapIn(scene, sub_map_no)) {
        return 1;
    }

    // Slot 1 is about to be loaded into the retail sub map stack and blocks, which slot 2 may be using.
    DropSeamless(scene);
    SlotOneSeamless = false;
    SeamlessFailed = false;

    char *map_name = GetMapName(sub_map_no, NULL);

    if (map_name == NULL) {
        printf("not found map %d\n", sub_map_no);
        return 0;
    }

    mgWaitFrame();
    scene->DeleteMap(PortSubMapInfo.map_no, 1);
    scene->DeleteSubVillager();
    SCN_LOADMAP_INFO2 info;
    char              map_path[0x40];
    char              file_name[0x20];
    info.tex_block = PortSubMapInfo.tex_block;
    info.stack_no = PortSubMapInfo.stack_no;
    info.load_buf = PortSubMapInfo.load_buf;
    info.efp_tex_block = PortSubMapInfo.efp_tex_block;
    strcpy(info.name, map_name);

    if (info.place_parts_max <= 0) {
        info.place_parts_max = 0x140;
    }

    GetMapPath(map_path, map_name);
    DivPathName(map_path, info.files[0].dir, file_name);
    info.files[0].enable = 1;
    strcpy(info.files[0].map_name, file_name);
    strcpy(info.files[0].cfg_name, file_name);
    strcpy(info.files[0].mpk_name, file_name);
    strcpy(info.files[0].ipk_name, file_name);
    strcpy(info.files[0].efp_name, file_name);
    strcpy(info.name, file_name);

    if (scene->LoadMap(PortSubMapInfo.map_no, &info, flag) < 0) {
        return 0;
    }

    CSaveData *save_data = GetSaveData();

    s16 *sub_map_nos = (&save_data->map_no);
    sub_map_nos[3] = PortNowSubMapNo;
    scene->SetNowSubMapNo(sub_map_no);
    PortNowSubMapNo = sub_map_no;
    sub_map_nos[1] = sub_map_no;
    return 1;
}

void TownSeamlessStep(CScene *scene) {
    Revalidate(scene);
    if (!ConfigGet().seamless_lake || !InPalmBrinks(scene)) {
        DropSeamless(scene);
        return;
    }

    const int partner = Partner(PortNowSubMapNo);
    if (SeamlessScene == scene && SeamlessMapNo >= 0) {
        if (SeamlessMapNo != partner) {
            DropSeamless(scene);
        }
        return;
    }

    // Slot 2 is loaded into SeamlessStack, so only while slot 1 is in the retail stack, and only between loads.
    if (partner < 0 || SlotOneSeamless || SeamlessFailed || !TableHasRoom() ||
        !SlotHolds(scene, kSubMapSlot, PortNowSubMapNo) || scene->GetSceneMap(kSeamlessSlot)->status != 0 ||
        scene->bg_load_step != 0 || PortSubMapLoadBG != 0 || scene->event_run) {
        return;
    }

    if (!LoadSeamless(scene, partner)) {
        SeamlessFailed = true;
        std::fprintf(stderr, "seamless lake: could not load sub map %d beside %d\n", partner, PortNowSubMapNo);
    }
}

void TownSeamlessBeforeClearStack(CScene *scene, int index) {
    Revalidate(scene);
    if (SeamlessScene != scene || SeamlessMapNo < 0) {
        return;
    }
    CSceneMap *slot = scene->GetSceneMap(kSeamlessSlot);
    for (int i = index; i < scene->stack_num; i++) {
        if (scene->stack[i] != NULL && scene->stack[i] == slot->stack) {
            DropSeamless(scene);
            return;
        }
    }
}
