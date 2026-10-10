#include "../gif/gif.h"
#include "vu1_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gfx {

Vu1Host::Vu1Host(Gif &gif) : gif_(gif), storage_(1) {
    vu_ = &storage_[0];
    std::memset(vu_, 0, sizeof(Vu1));
}

void Vu1Host::Register(uint32_t load_addr, const uint64_t *code, size_t count, IVu1Program *program) {
    entries_.push_back(Entry{load_addr, std::vector<uint64_t>(code, code + count), program});
    loaded_dirty_ = true;
}

void Vu1Host::LoadMicro(uint32_t addr, const uint64_t *code, size_t count) {
    for (size_t i = 0; i < count; i++) {
        vu_->micro[(addr + i) & 0x7FF] = code[i];
    }
    loaded_dirty_ = true;
}

IVu1Program *Vu1Host::Find(uint32_t pc) {
    if (loaded_dirty_) {
        loaded_.clear();
        for (const Entry &e : entries_) {
            if (e.load_addr + e.code.size() > 2048) {
                continue;
            }
            if (std::memcmp(&vu_->micro[e.load_addr], e.code.data(), e.code.size() * 8) == 0) {
                loaded_.push_back(&e);
            }
        }
        // When several match (a signature inside a longer program), the longest wins.
        std::sort(loaded_.begin(), loaded_.end(),
                  [](const Entry *a, const Entry *b) { return a->code.size() > b->code.size(); });
        loaded_dirty_ = false;
    }
    for (const Entry *e : loaded_) {
        if (pc >= e->load_addr && pc < e->load_addr + e->code.size()) {
            return e->program;
        }
    }
    // A signature only covers the start of its program; any address past it
    // still belongs to the program loaded at the nearest lower address.
    const Entry *best = nullptr;
    for (const Entry *e : loaded_) {
        if (e->load_addr <= pc && (!best || e->load_addr > best->load_addr)) {
            best = e;
        }
    }
    return best ? best->program : fallback_;
}

uint32_t Vu1Host::Call(uint32_t pc) {
    IVu1Program *program = Find(pc & 0x7FF);
    if (program == nullptr) {
        if (std::find(unhandled_.begin(), unhandled_.end(), pc) == unhandled_.end()) {
            unhandled_.push_back(pc);
            std::fprintf(stderr, "gfx: no VU1 program at 0x%03X, MSCAL ignored\n", pc);
        }
        return pc;
    }
    return program->Run(*this, pc & 0x7FF);
}

void Vu1Host::Start(uint32_t pc) {
    tpc_ = Call(pc);
}

void Vu1Host::Kick(uint32_t addr) {
    gif_.Kick(vu_->mem, addr);
}

void Vu1Host::Kick(const uint32_t (*mem)[4], uint32_t addr) {
    gif_.Kick(mem, addr);
}

} // namespace gfx
