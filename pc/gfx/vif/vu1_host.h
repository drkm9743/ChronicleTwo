// VU1 as the VIF and the GIF see it: 16 KiB of data memory, 16 KiB of micro
// memory, and a way to run whatever microprogram is loaded. There is no VU
// interpreter here: microprograms are C++ functions (IVu1Program) registered
// with the code they stand for, and MSCAL runs the one whose code is loaded at
// the address called. A reference interpreter can be plugged in as the
// fallback for code nobody registered.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gfx {

class Gif;
class Vu1Host;

/** The VU1 state a microprogram reads and writes. */
struct Vu1 {
    alignas(16) uint32_t mem[1024][4]; // data memory, by quadword
    uint64_t micro[2048];              // micro memory, by instruction pair
    uint32_t top;                      // TOP/ITOP as the VIF set them when the program started
    uint32_t itop;
    // Registers a program may keep between calls (Vu_progmain leaves the
    // model-to-screen matrix in vf10-vf13 for the overlay that follows).
    alignas(16) float vf[32][4];
    int32_t vi[16];

    float *MemF(uint32_t qw) { return reinterpret_cast<float *>(mem[qw & 0x3FF]); }
};

class IVu1Program {
public:
    virtual ~IVu1Program() = default;

    /**
     * Runs from instruction `pc` (in 64-bit units, as MSCAL gives it) until the
     * program ends. Returns the instruction MSCNT resumes from.
     */
    virtual uint32_t Run(Vu1Host &host, uint32_t pc) = 0;
};

class Vu1Host {
public:
    explicit Vu1Host(Gif &gif);

    Vu1 &State() { return *vu_; }

    /**
     * Registers a program as the code it replaces: `count` instruction pairs
     * loaded at `load_addr`. The program runs when micro memory holds exactly
     * that code and MSCAL or MSCNT lands inside it. The code may be a short
     * signature rather than the real microcode, as long as what the game
     * uploads matches it.
     */
    void Register(uint32_t load_addr, const uint64_t *code, size_t count, IVu1Program *program);

    /** Runs for MSCAL/MSCNT addresses no registered program covers. */
    void SetFallback(IVu1Program *program) { fallback_ = program; }

    /** MPG: writes instruction pairs into micro memory. */
    void LoadMicro(uint32_t addr, const uint64_t *code, size_t count);

    /** MSCAL/MSCALF: runs from `pc`. */
    void Start(uint32_t pc);

    /** MSCNT: resumes where the last program ended. */
    void Continue() { Start(tpc_); }

    /** Lets a program call into another (Vu_progmain's MSCAL 2 entry jumps to the overlay at 0x064). */
    uint32_t Call(uint32_t pc);

    /** XGKICK: sends the GIF packet at data-memory quadword `addr` down PATH1. */
    void Kick(uint32_t addr);

    /** Addresses MSCAL reached with nothing to run, for diagnostics. */
    const std::vector<uint32_t> &Unhandled() const { return unhandled_; }

private:
    struct Entry {
        uint32_t              load_addr;
        std::vector<uint64_t> code;
        IVu1Program          *program;
    };

    IVu1Program *Find(uint32_t pc);

    Gif                       &gif_;
    std::vector<Vu1>           storage_;
    Vu1                       *vu_;
    std::vector<Entry>         entries_;
    std::vector<const Entry *> loaded_;
    bool                       loaded_dirty_ = true;
    IVu1Program               *fallback_ = nullptr;
    uint32_t                   tpc_ = 0;
    std::vector<uint32_t>      unhandled_;
};

} // namespace gfx
