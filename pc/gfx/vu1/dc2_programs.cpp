#include "dc2_programs.h"
#include "vu_progs.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)
#include <xmmintrin.h>
#define DC2_HAVE_MXCSR 1
#endif

namespace gfx {

namespace {

/** Where Vu_progmain jumps for the overlay, and how much of micro memory is progmain's. */
const uint32_t kOverlayAddr = 0x064;

/** What all the programs share between calls: Vu_progmain's results and the output buffer (vi13). */
struct Shared {
    alignas(16) Vu1State state;
    Vu1Host *host = nullptr;
};

Shared &SharedState() {
    static Shared *shared = [] {
        Shared *s = new Shared();
        std::memset(&s->state, 0, sizeof(s->state));
        s->state.out_buffer = 0x1A4;
        return s;
    }();
    return *shared;
}

void KickFromState(const Vec4 *packet) {
    Shared &s = SharedState();
    s.host->Kick(reinterpret_cast<const uint32_t(*)[4]>(s.state.mem), uint32_t(packet - s.state.mem));
}

/** Round toward zero, flush and treat denormals as zero, for the length of one call. */
class VuFloatMode {
public:
    VuFloatMode() {
#ifdef DC2_HAVE_MXCSR
        saved_ = _mm_getcsr();
        _mm_setcsr((saved_ & ~0x6000u) | 0x6000u | 0x8040u);
#endif
    }

    ~VuFloatMode() {
#ifdef DC2_HAVE_MXCSR
        _mm_setcsr(saved_);
#endif
    }

private:
    unsigned saved_ = 0;
};

/** Runs `fn` on a copy of VU1 data memory and writes the copy back. */
template <typename Fn>
void RunOnState(Vu1Host &host, Fn fn) {
    Shared &s = SharedState();
    Vu1    &vu = host.State();
    s.host = &host;
    std::memcpy(s.state.mem, vu.mem, sizeof(s.state.mem));
    s.state.top = uint16_t(vu.top);
    {
        VuFloatMode mode;
        fn(s.state);
    }
    std::memcpy(vu.mem, s.state.mem, sizeof(vu.mem));
}

class ProgMain : public IVu1Program {
public:
    uint32_t Run(Vu1Host &host, uint32_t pc) override {
        if (pc == 0) {
            RunOnState(host, [](Vu1State &state) { VuProgMainSetup(state); });
            return 0;
        }
        // The other entries set up for the overlay and jump to it.
        return host.Call(kOverlayAddr);
    }
};

class Overlay : public IVu1Program {
public:
    using Function = void (*)(Vu1State &, void (*)(const Vec4 *));

    explicit Overlay(Function fn) : fn_(fn) {}

    uint32_t Run(Vu1Host &host, uint32_t) override {
        Function fn = fn_;
        RunOnState(host, [fn](Vu1State &state) { fn(state, KickFromState); });
        // MSCNT goes back into the overlay.
        return kOverlayAddr;
    }

private:
    Function fn_;
};

/** The code an upload packet puts in micro memory: a CNT/END tag whose upper half and data are VIF codes. */
bool ParseMpg(const void *packet, uint32_t *load_addr, std::vector<uint64_t> *code) {
    const uint8_t *p = static_cast<const uint8_t *>(packet);
    uint32_t       tag;
    std::memcpy(&tag, p, 4);
    const uint8_t *end = p + 16 + size_t(tag & 0xFFFF) * 16;
    const uint8_t *pos = p + 8;
    bool           found = false;
    uint32_t       next = 0;
    while (pos + 4 <= end) {
        uint32_t vif;
        std::memcpy(&vif, pos, 4);
        pos += 4;
        uint32_t cmd = (vif >> 24) & 0x7F, num = (vif >> 16) & 0xFF, imm = vif & 0xFFFF;
        if (cmd != 0x4A) {
            continue;
        }
        uint32_t n = num ? num : 256;
        pos = p + ((pos - p + 7) & ~ptrdiff_t(7));
        if (!found) {
            *load_addr = imm;
            next = imm;
            found = true;
        }
        if (imm != next) {
            break; // a second, separate upload: keep the first run
        }
        for (uint32_t i = 0; i < n; i++) {
            uint64_t pair;
            std::memcpy(&pair, pos + i * 8, 8);
            code->push_back(pair);
        }
        pos += size_t(n) * 8;
        next = imm + n;
    }
    return found;
}

bool RegisterPacket(Vu1Host &host, const void *packet, size_t max_pairs, IVu1Program *program) {
    uint32_t              addr = 0;
    std::vector<uint64_t> code;
    if (packet == nullptr || !ParseMpg(packet, &addr, &code)) {
        return false;
    }
    if (code.size() > max_pairs) {
        code.resize(max_pairs);
    }
    while (!code.empty() && code.back() == 0) {
        code.pop_back();
    }
    host.Register(addr, code.data(), code.size(), program);
    return true;
}

} // namespace

int RegisterDc2Programs(Vu1Host &host, const Dc2ProgramPackets &packets) {
    static ProgMain progmain;
    static Overlay  model(VuProgModel), shadow(VuProgShadow), sprite3d(VuProg3DSprite), water(VuProgWater);
    int             count = 0;
    // Only progmain's own part: the overlays load over whatever follows it.
    count += RegisterPacket(host, packets.progmain, kOverlayAddr, &progmain);
    count += RegisterPacket(host, packets.model, 2048, &model);
    count += RegisterPacket(host, packets.shadow, 2048, &shadow);
    count += RegisterPacket(host, packets.sprite3d, 2048, &sprite3d);
    count += RegisterPacket(host, packets.water, 2048, &water);
    return count;
}

} // namespace gfx
