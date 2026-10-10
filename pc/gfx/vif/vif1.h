// VIF1: decodes the VIF code stream that DMA channel 1 delivers (with the DMA
// tags' upper halves when TTE is set) and carries it out against VU1 memory,
// the VU1 programs and GIF PATH2. Codes and their data may arrive split across
// any number of writes.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

class Gif;
class Vu1Host;

class Vif1 {
public:
    Vif1(Vu1Host &vu, Gif &gif);

    /** Feeds `count` 32-bit words of VIF codes and data. */
    void Write(const uint32_t *words, size_t count);

    void Reset();

    uint32_t Row(int i) const { return row_[i]; }

    uint32_t Top() const { return tops_; }

private:
    enum Pending {
        NONE,
        STMASK,
        STROW,
        STCOL,
        MPG,
        DIRECT,
        UNPACK,
    };

    void Code(uint32_t code);
    void Data(uint32_t word);
    void StartUnpack(uint32_t code);
    /** Writes as many unpacked vectors as the buffered data allows. */
    void RunUnpack();
    void WriteVector(uint32_t n, const uint32_t *data, bool has_data);
    void StartProgram(uint32_t pc, bool resume);

    Vu1Host &vu_;
    Gif     &gif_;

    // Registers.
    uint32_t cl_ = 1, wl_ = 1;
    uint32_t mode_ = 0;
    uint32_t mask_ = 0;
    uint32_t row_[4] = {};
    uint32_t col_[4] = {};
    uint32_t base_ = 0, ofst_ = 0, tops_ = 0, itop_ = 0, itops_ = 0;
    bool     dbf_ = false;

    // The code being fed data.
    Pending  pending_ = NONE;
    uint32_t words_left_ = 0;
    uint32_t index_ = 0;
    uint32_t mpg_addr_ = 0;
    uint32_t mpg_low_ = 0;
    uint32_t direct_qw_[4] = {};

    struct Unpack {
        uint32_t addr = 0;    // first quadword written
        uint32_t num = 0;     // vectors to write
        uint32_t written = 0; // vectors written so far
        uint32_t vn = 0, vl = 0;
        bool     masked = false;
        bool     usn = false;
        uint32_t vector_bytes = 0;
        uint8_t  buffer[32] = {};
        uint32_t buffered = 0;
    } unpack_;
};

} // namespace gfx
