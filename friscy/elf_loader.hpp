// elf_loader.hpp - ELF binary loader with dynamic linking support
//
// This handles loading dynamically-linked RISC-V binaries by:
// 1. Detecting PT_INTERP (dynamic linker path)
// 2. Loading the interpreter as the actual entry point
// 3. Setting up the auxiliary vector for the dynamic linker
//
// This allows running standard dynamically-linked containers without
// requiring --static compilation.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <stdexcept>

namespace elf {

// ELF64 header structures (RISC-V specific)
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF constants
constexpr uint32_t PT_NULL    = 0;
constexpr uint32_t PT_LOAD    = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_INTERP  = 3;
constexpr uint32_t PT_NOTE    = 4;
constexpr uint32_t PT_PHDR    = 6;

constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN  = 3;

constexpr uint16_t EM_RISCV = 0xF3;

// Auxiliary vector types (for dynamic linker)
constexpr uint64_t AT_NULL         = 0;
constexpr uint64_t AT_IGNORE       = 1;
constexpr uint64_t AT_EXECFD       = 2;
constexpr uint64_t AT_PHDR         = 3;
constexpr uint64_t AT_PHENT        = 4;
constexpr uint64_t AT_PHNUM        = 5;
constexpr uint64_t AT_PAGESZ       = 6;
constexpr uint64_t AT_BASE         = 7;
constexpr uint64_t AT_FLAGS        = 8;
constexpr uint64_t AT_ENTRY        = 9;
constexpr uint64_t AT_NOTELF       = 10;
constexpr uint64_t AT_UID          = 11;
constexpr uint64_t AT_EUID         = 12;
constexpr uint64_t AT_GID          = 13;
constexpr uint64_t AT_EGID         = 14;
constexpr uint64_t AT_PLATFORM     = 15;
constexpr uint64_t AT_HWCAP        = 16;
constexpr uint64_t AT_CLKTCK       = 17;
constexpr uint64_t AT_SECURE       = 23;
constexpr uint64_t AT_BASE_PLATFORM = 24;
constexpr uint64_t AT_RANDOM       = 25;
constexpr uint64_t AT_HWCAP2       = 26;
constexpr uint64_t AT_EXECFN       = 31;

// RISC-V hardware capabilities
constexpr uint64_t RISCV_HWCAP_IMAFDC = 0x112D;  // I, M, A, F, D, C extensions

// Information about a loaded ELF
struct ElfInfo {
    uint64_t entry_point;      // e_entry
    uint64_t phdr_addr;        // Address of program headers in memory
    uint16_t phdr_size;        // Size of one program header
    uint16_t phdr_count;       // Number of program headers
    uint64_t base_addr;        // Load base (0 for ET_EXEC, varies for ET_DYN)
    bool is_dynamic;           // Has PT_INTERP
    std::string interpreter;   // Path to dynamic linker
    uint16_t type;             // ET_EXEC or ET_DYN
};

// Parse ELF header and program headers
inline ElfInfo parse_elf(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(Elf64_Ehdr)) {
        throw std::runtime_error("ELF too small");
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());

    // Validate magic
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        throw std::runtime_error("Not an ELF file");
    }

    // Check 64-bit
    if (ehdr->e_ident[4] != 2) {
        throw std::runtime_error("Not a 64-bit ELF");
    }

    // Check RISC-V
    if (ehdr->e_machine != EM_RISCV) {
        throw std::runtime_error("Not a RISC-V ELF");
    }

    // Check type
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        throw std::runtime_error("ELF is not executable or shared object");
    }

    ElfInfo info;
    info.entry_point = ehdr->e_entry;
    info.phdr_size = ehdr->e_phentsize;
    info.phdr_count = ehdr->e_phnum;
    info.type = ehdr->e_type;
    info.is_dynamic = false;
    info.base_addr = 0;

    // Find PT_PHDR and PT_INTERP
    uint64_t phdr_vaddr = 0;
    size_t phoff = ehdr->e_phoff;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phoff + sizeof(Elf64_Phdr) > data.size()) break;

        const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data.data() + phoff);

        if (phdr->p_type == PT_PHDR) {
            phdr_vaddr = phdr->p_vaddr;
        }
        else if (phdr->p_type == PT_INTERP) {
            info.is_dynamic = true;
            // Extract interpreter path
            if (phdr->p_offset + phdr->p_filesz <= data.size()) {
                info.interpreter = std::string(
                    reinterpret_cast<const char*>(data.data() + phdr->p_offset),
                    phdr->p_filesz
                );
                // Remove trailing null
                while (!info.interpreter.empty() && info.interpreter.back() == '\0') {
                    info.interpreter.pop_back();
                }
            }
        }

        phoff += ehdr->e_phentsize;
    }

    // If no PT_PHDR, calculate from first PT_LOAD
    if (phdr_vaddr == 0) {
        phoff = ehdr->e_phoff;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data.data() + phoff);
            if (phdr->p_type == PT_LOAD && phdr->p_offset == 0) {
                // Program headers are in this segment
                phdr_vaddr = phdr->p_vaddr + ehdr->e_phoff;
                break;
            }
            phoff += ehdr->e_phentsize;
        }
    }

    info.phdr_addr = phdr_vaddr;

    return info;
}

// Get the lowest and highest virtual addresses from PT_LOAD segments
inline std::pair<uint64_t, uint64_t> get_load_range(const std::vector<uint8_t>& data) {
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());

    uint64_t lo = UINT64_MAX;
    uint64_t hi = 0;

    size_t phoff = ehdr->e_phoff;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data.data() + phoff);

        if (phdr->p_type == PT_LOAD) {
            uint64_t seg_lo = phdr->p_vaddr;
            uint64_t seg_hi = phdr->p_vaddr + phdr->p_memsz;

            if (seg_lo < lo) lo = seg_lo;
            if (seg_hi > hi) hi = seg_hi;
        }

        phoff += ehdr->e_phentsize;
    }

    return {lo, hi};
}

// Build auxiliary vector for dynamic linker
// Returns pairs of (type, value) that should be pushed to stack
inline std::vector<std::pair<uint64_t, uint64_t>> build_auxv(
    const ElfInfo& exec_info,      // Main executable info
    const ElfInfo& interp_info,    // Interpreter info (if dynamic)
    uint64_t interp_base,          // Base address where interpreter was loaded
    uint64_t random_addr,          // Address of 16 random bytes
    uint64_t execfn_addr           // Address of executable filename string
) {
    std::vector<std::pair<uint64_t, uint64_t>> auxv;

    // Program headers of the main executable
    auxv.push_back({AT_PHDR,  exec_info.phdr_addr});
    auxv.push_back({AT_PHENT, exec_info.phdr_size});
    auxv.push_back({AT_PHNUM, exec_info.phdr_count});

    // Page size
    auxv.push_back({AT_PAGESZ, 4096});

    // Interpreter base address (only if dynamic)
    if (exec_info.is_dynamic) {
        auxv.push_back({AT_BASE, interp_base});
    } else {
        auxv.push_back({AT_BASE, 0});
    }

    // Entry point of the main executable (not interpreter)
    auxv.push_back({AT_ENTRY, exec_info.entry_point});

    // User/group IDs
    auxv.push_back({AT_UID,  0});
    auxv.push_back({AT_EUID, 0});
    auxv.push_back({AT_GID,  0});
    auxv.push_back({AT_EGID, 0});

    // Clock ticks per second
    auxv.push_back({AT_CLKTCK, 100});

    // Security mode (not secure)
    auxv.push_back({AT_SECURE, 0});

    // Hardware capabilities (IMAFDC)
    auxv.push_back({AT_HWCAP, RISCV_HWCAP_IMAFDC});

    // Random bytes for stack canary, etc.
    auxv.push_back({AT_RANDOM, random_addr});

    // Executable filename
    auxv.push_back({AT_EXECFN, execfn_addr});

    // Platform string (pointer to "riscv64")
    // We'll need to allocate this on the stack too
    auxv.push_back({AT_PLATFORM, 0});  // Will be filled in by caller

    // Terminator
    auxv.push_back({AT_NULL, 0});

    return auxv;
}

}  // namespace elf
