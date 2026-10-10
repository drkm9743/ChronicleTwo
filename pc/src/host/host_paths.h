#pragma once

// Where the PC build finds the disc, and where it keeps what the PS2 kept on
// devices of its own: the memory cards and the host0: files of the
// development tools.
//
// Each path comes from a setter when the executable calls one, then from an
// environment variable, then from a default:
//
//   disc         DC2_DISC     an ISO image, or a folder holding the extracted
//                             disc; "disc" beside the executable
//   host0:       DC2_HOST     "host" beside the executable
//   memory card  DC2_MEMCARD  "memcard" in the user's SDL preference folder

#include <filesystem>
#include <string>

namespace pc {

std::filesystem::path DiscPath();
std::filesystem::path HostPath();
std::filesystem::path MemoryCardPath();

void SetDiscPath(const std::filesystem::path &path);
void SetHostPath(const std::filesystem::path &path);
void SetMemoryCardPath(const std::filesystem::path &path);

/** Turns UTF-8 text into a path, which on Windows is UTF-16. */
std::filesystem::path PathFromUtf8(const std::string &text);

/** Turns a path into UTF-8 text. */
std::string Utf8FromPath(const std::filesystem::path &path);

/** Compares two names as the disc and the memory card do: ASCII letters in either case match. */
bool NamesEqualNoCase(const std::string &a, const std::string &b);

/**
 * Finds `relative` (components split by '/' or '\\') under `root`, matching
 * each component in either case, as the PS2 devices do. Gives true and the
 * host path when it exists; otherwise false, with `out` set to the path the
 * name would take, keeping the case of the components that do exist.
 */
bool ResolveNoCase(const std::filesystem::path &root, const std::string &relative,
                   std::filesystem::path *out);

} // namespace pc
