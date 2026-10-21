// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 Linux ELF64 executable inspector, segment mapper, and System V AMD64
// initial process stack builder.
//
// Security rules (SECURITY.md):
//   - Input bytes are untrusted: every offset, program header table entry,
//     p_filesz/p_memsz bound, and virtual address range is checked for
//     truncation and integer overflow before touching host memory.
//   - Dynamic binaries requesting a runtime linker (`PT_INTERP`) are reported
//     explicitly (`ElfError::kNeedsInterpreter`) rather than jumping to _start
//     with unresolved GOT/PLT relocations.
#ifndef HYPERION_ELF_LOADER_HPP
#define HYPERION_ELF_LOADER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyperion/address_space.hpp"

namespace hyperion {

enum class ElfError : std::uint8_t {
    kNone = 0,
    kNotElf,
    kTruncated,
    kUnsupportedClass,   // not ELFCLASS64 or not little-endian
    kUnsupportedMachine, // not EM_X86_64 (62)
    kUnsupportedType,    // neither ET_EXEC (2) nor ET_DYN (3)
    kNeedsInterpreter,   // PT_INTERP present (dynamic linking not yet implemented)
    kMalformedProgramHeaders,
    kMappingFailed,
};

const char* to_string(ElfError error);

struct ElfSegmentInfo {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;
    std::uint64_t vaddr = 0;
    std::uint64_t filesz = 0;
    std::uint64_t memsz = 0;
    std::uint64_t align = 0;
};

struct ElfInfo {
    ElfError error = ElfError::kNone;
    std::uint16_t type = 0;    // ET_EXEC (2) or ET_DYN (3)
    std::uint16_t machine = 0; // EM_X86_64 (62)
    std::uint64_t entry = 0;
    std::uint64_t phoff = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
    bool has_interp = false;
    std::string interpreter;
    std::vector<ElfSegmentInfo> segments;

    bool ok() const noexcept { return error == ElfError::kNone; }
};

struct LoadedElf {
    ElfError error = ElfError::kNone;
    std::uint64_t entry = 0;
    std::uint64_t load_bias = 0;
    std::uint64_t phdr_vaddr = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
    std::uint64_t brk_hint = 0;

    bool ok() const noexcept { return error == ElfError::kNone; }
};

// Returns true if `bytes` starts with the 4-byte `\x7fELF` magic.
bool has_elf_magic(const std::uint8_t* bytes, std::size_t size) noexcept;

// Parse and validate an ELF64 x86-64 executable image without mapping it.
// Note: `has_interp` is populated for inspection (`hyperion info`), while
// `load_elf_image` rejects `PT_INTERP` with `ElfError::kNeedsInterpreter`.
ElfInfo inspect_elf(const std::uint8_t* bytes, std::size_t size);

// Format human-readable summary of `info` for `hyperion info <path>`.
std::string format_elf_info(const ElfInfo& info, const std::string& path);

// Map all `PT_LOAD` segments of a static x86-64 `ET_EXEC` or static PIE
// `ET_DYN` image into `address_space` with page-level `PF_R|PF_W|PF_X`
// enforcement and zero-filled `.bss`.
LoadedElf load_elf_image(const std::uint8_t* bytes, std::size_t size,
                         GuestAddressSpace& address_space,
                         std::uint64_t pie_load_base = 0x400000ULL);

// Map a read/write guest stack and lay out the System V AMD64 initial process
// stack frame (`argc`, `argv[]`, `NULL`, `envp[]`, `NULL`, `auxv[]`), returning
// the 16-byte-aligned initial `%rsp` value (or 0 on failure).
std::uint64_t setup_linux_x86_64_stack(GuestAddressSpace& address_space,
                                       const std::vector<std::string>& argv,
                                       const LoadedElf* loaded_elf = nullptr,
                                       std::uint64_t stack_top = 0x7fffffffe000ULL,
                                       std::size_t stack_bytes = 64ULL * 1024ULL);

} // namespace hyperion

#endif // HYPERION_ELF_LOADER_HPP
