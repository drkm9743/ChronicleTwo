#include "common.h"

#include "mg_memory.hpp"
#include "scene.hpp"
#include "scenesnd.hpp"
#include "town_seamless.hpp"

// Stride fix: retail walks stack[] as (u8 *) this + 8 + i * 4, a 4-byte pointer stride.
PORT_OVERRIDE void CScene::ClearStack(int index) {
    TownSeamlessBeforeClearStack(this, index);

    for (int i = index; i < stack_num; i++) {
        mgCMemory *memory = stack[i];

        if (memory == NULL) {
            continue;
        }

        memory->stack_used = 0;
        memory->lock = 0;

        if (index < i) {
            memory->stSetBuffer(NULL, 0);
        }
    }
}
