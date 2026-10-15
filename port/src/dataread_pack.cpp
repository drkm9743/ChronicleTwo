#include "common.h"

#include "dataread.hpp"
#include "platform/texture_catalog.hpp"
#include "texture_source.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

bool EqualName(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

std::string CatalogMemberName(std::string_view name) {
    std::string result;
    for (unsigned char value : name) {
        if (value >= 128 || value == '%') {
            static constexpr char hex[] = "0123456789ABCDEF";
            result += '%';
            result += hex[value >> 4];
            result += hex[value & 15];
        } else {
            result += static_cast<char>(value == '\\' ? '/' : value);
        }
    }
    return result;
}

u_int *RegisterMember(u_int *pack, PACK_ENTRY *entry, int *out_size) {
    auto *data = reinterpret_cast<u_int *>(reinterpret_cast<u8 *>(entry) + entry->offset);
    if (auto parent = TextureSourceLookup(pack)) {
        std::string source = *parent + "/" + CatalogMemberName(entry->name);
        bool converted = texture_catalog::Contains(source);
        if (!converted) {
            // A retail PACK can hold a zero-sized member (dungeon/monster/e03a.chr's e03a.wgt); the
            // converter then reads the whole file as raw bytes and names each image it finds by its
            // byte offset in the parent, which is where this member's data sits.
            const auto offset = reinterpret_cast<u8 *>(data) - reinterpret_cast<u8 *>(pack);
            std::string by_offset = *parent + "#offset:" + std::to_string(offset);
            if (texture_catalog::Contains(by_offset)) {
                source = std::move(by_offset);
                converted = true;
            }
        }
        if (converted) {
            // The published PACK has no texture payload. Give callers a stable, opaque in-memory
            // handle that can be copied into their arena without resembling an IMG or TIM2 header.
            static std::unordered_map<std::string, std::array<u_int, 4>> handles;
            auto &handle = handles[source];
            data = handle.data();
            if (out_size) {
                *out_size = sizeof(handle);
            }
        } else if (out_size) {
            *out_size = entry->size;
        }
        TextureSourceRegister(data, std::move(source),
                              converted ? sizeof(std::array<u_int, 4>) : static_cast<std::size_t>(entry->size));
    } else if (out_size) {
        *out_size = entry->size;
    }
    return data;
}

} // namespace

PORT_OVERRIDE u_int *GetPackFile(u_int *pack, char *name, int *out_size) {
    if (!pack || !name || !*name) {
        return nullptr;
    }
    const char *base = name;
    for (const char *scan = name; *scan; ++scan) {
        if (*scan == '/' || *scan == '\\') {
            base = scan + 1;
        }
    }
    for (auto *entry = reinterpret_cast<PACK_ENTRY *>(pack); entry->name[0];
         entry = reinterpret_cast<PACK_ENTRY *>(reinterpret_cast<u8 *>(entry) + entry->next)) {
        if (EqualName(entry->name, base)) {
            return RegisterMember(pack, entry, out_size);
        }
    }
    return nullptr;
}

PORT_OVERRIDE u_int *GetPackFile(u_int *pack, int index, char **out_name, int *out_size) {
    if (!pack || index < 0) {
        return nullptr;
    }
    auto *entry = reinterpret_cast<PACK_ENTRY *>(pack);
    for (int i = 0; entry->name[0]; ++i,
             entry = reinterpret_cast<PACK_ENTRY *>(reinterpret_cast<u8 *>(entry) + entry->next)) {
        if (i != index) {
            continue;
        }
        if (out_name) {
            *out_name = entry->name;
        }
        return RegisterMember(pack, entry, out_size);
    }
    return nullptr;
}
