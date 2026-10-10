// libsdr on the PC: each sceSdRemote command is the libsd call it names,
// run on the software SPU2 at once.

#include "../host/sdk.h"
#include "../iop/iop.h"
#include "servers.h"
#include "spu2.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdarg>
#include <mutex>

namespace {

// Commands beyond the ones libsdr.h names.
enum Command {
    SD_GET_PARAM = 0x8020,
    SD_GET_SWITCH = 0x8040,
    SD_GET_ADDR = 0x8060,
    SD_GET_CORE_ATTR = 0x8080,
    SD_NOTE_TO_PITCH = 0x8090,
    SD_PITCH_TO_NOTE = 0x80A0,
    SD_BLOCK_TRANS = 0x80E0,
    SD_BLOCK_TRANS_STATUS = 0x8100,
};

/** Bits of a transfer mode. */
const int kTransModeMask = 0x03;
const int kTransRead = 1;
const int kTransStop = 2;
const int kTransWriteFrom = 3;
const int kBlockLoop = 0x10;

/** sceSdEffectAttr's reverb types are in the low byte of `mode`. */
const int kEffectModeMask = 0xFF;

int NoteToPitch(int center_note, int center_fine, int note, int fine) {
    double semitones = (note - center_note) + (fine - center_fine) / 128.0;
    double pitch = 4096.0 * std::pow(2.0, semitones / 12.0);
    return (int) std::min(pitch + 0.5, 16383.0);
}

int PitchToNote(int center_note, int center_fine, int pitch) {
    double semitones = 12.0 * std::log2(std::max(pitch, 1) / 4096.0) + center_note + center_fine / 128.0;
    int    fine = (int) std::floor(semitones * 128.0 + 0.5);
    return ((fine / 128) << 8) | (fine % 128);
}

} // namespace

extern "C" {

int sceSdRemoteInit(void) {
    pc::audio::StartOutput();
    return 0;
}

int sceSdRemote(int arg, int command, ...) {
    (void) arg;
    std::lock_guard<std::recursive_mutex> guard(pc::iop::Lock());
    pc::audio::Spu2                      &spu = pc::audio::GetSpu();
    va_list                               args;
    int                                   result = 0;

    va_start(args, command);

    switch (command) {
        case rSdInit:
            spu.Reset();
            break;
        case rSdSetParam: {
            int entry = va_arg(args, int);
            spu.SetParam((uint16_t) entry, (uint16_t) va_arg(args, int));
            break;
        }
        case SD_GET_PARAM:
            result = spu.GetParam((uint16_t) va_arg(args, int));
            break;
        case rSdSetSwitch: {
            int entry = va_arg(args, int);
            spu.SetSwitch((uint16_t) entry, (uint32_t) va_arg(args, int));
            break;
        }
        case SD_GET_SWITCH:
            result = (int) spu.GetSwitch((uint16_t) va_arg(args, int));
            break;
        case rSdSetAddr: {
            int entry = va_arg(args, int);
            spu.SetAddr((uint16_t) entry, (uint32_t) va_arg(args, int));
            break;
        }
        case SD_GET_ADDR:
            result = (int) spu.GetAddr((uint16_t) va_arg(args, int));
            break;
        case rSdSetCoreAttr: {
            int entry = va_arg(args, int);
            spu.SetCoreAttr((uint16_t) entry, (uint16_t) va_arg(args, int));
            break;
        }
        case SD_GET_CORE_ATTR:
            result = spu.GetCoreAttr((uint16_t) va_arg(args, int));
            break;
        case SD_NOTE_TO_PITCH: {
            int center_note = va_arg(args, int);
            int center_fine = va_arg(args, int);
            int note = va_arg(args, int);
            result = NoteToPitch(center_note, center_fine, note, va_arg(args, int));
            break;
        }
        case SD_PITCH_TO_NOTE: {
            int center_note = va_arg(args, int);
            int center_fine = va_arg(args, int);
            result = PitchToNote(center_note, center_fine, va_arg(args, int));
            break;
        }
        case rSdVoiceTrans: {
            // (channel, mode, IOP address, SPU address, size); the game passes the IOP address
            // as a pointer or an int, and only its low 32 bits mean anything.
            int          channel = va_arg(args, int);
            int          mode = va_arg(args, int);
            unsigned int iop_address = va_arg(args, unsigned int);
            unsigned int spu_address = va_arg(args, unsigned int);
            unsigned int size = va_arg(args, unsigned int);
            void        *memory = pc::iop::Translate(iop_address, size);

            (void) channel;

            if (memory == NULL) {
                SDL_Log("sceSdVoiceTrans from 0x%X (%u bytes) is outside IOP memory", iop_address, size);
                result = -1;
            } else if ((mode & kTransModeMask) == kTransRead) {
                spu.ReadRam(spu_address, memory, size);
                result = (int) size;
            } else {
                spu.WriteRam(spu_address, memory, size);
                result = (int) size;
            }

            break;
        }
        case rSdVoiceTransStatus:
            // Transfers finish as they start.
            result = 1;
            break;
        case SD_BLOCK_TRANS: {
            int channel = va_arg(args, int);
            int mode = va_arg(args, int);

            if ((mode & kTransModeMask) == kTransStop) {
                result = (int) (spu.StopBlock(channel) & 0xFFFFFF);
                break;
            }

            unsigned int iop_address = va_arg(args, unsigned int);
            unsigned int size = va_arg(args, unsigned int);
            unsigned int start = (mode & kTransModeMask) == kTransWriteFrom ? va_arg(args, unsigned int) : iop_address;
            spu.StartBlock(channel, iop_address, size, start, (mode & kBlockLoop) != 0);
            break;
        }
        case SD_BLOCK_TRANS_STATUS:
            result = (int) spu.BlockStatus(va_arg(args, int));
            break;
        case rSdSetEffectAttr: {
            int                    core = va_arg(args, int);
            const sceSdEffectAttr *attr = va_arg(args, const sceSdEffectAttr *);
            spu.SetEffect(core, attr->mode & kEffectModeMask, attr->depth_L, attr->depth_R, attr->delay,
                          attr->feedback);
            break;
        }
        default:
            SDL_Log("sceSdRemote command 0x%X is not implemented", command);
            break;
    }

    va_end(args);
    return result;
}

} // extern "C"
