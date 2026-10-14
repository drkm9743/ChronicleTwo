#pragma once

#include "scene.hpp"
#include "scenesnd.hpp"

// Palm Brinks keeps one sub map at a time in scene slot 1, and swaps it for another as the player walks. Retail
// hides the swap between m04 and the lakeside m05 behind a raised, locked camera. The port keeps whichever of the
// two is not in slot 1 loaded in slot 2, in its own stack and texture blocks, so both are drawn together; walking
// across swaps the two slots instead of loading.

// The town's texture table (editloop's SetTableBuffer(350, 221, ...)) and the room added to it for slot 2.
inline constexpr int kTownTextureCount = 350;
inline constexpr int kTownTextureBlocks = 221;
inline constexpr int kSeamlessTextureCount = 350;
inline constexpr int kSeamlessTextureBlocks = 64;

// Called once a frame from the town walk control.
void TownSeamlessStep(CScene *scene);
// Called before CScene::ClearStack clears index and the stacks after it.
void TownSeamlessBeforeClearStack(CScene *scene, int index);
