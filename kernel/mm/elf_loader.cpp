#include <mm/elf_loader.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <nord/types.hpp>

namespace nord::mm::elf_loader {

static constexpr u32 PT_LOAD = 1;
static constexpr u32 PF_X = 1;
static constexpr u32 PF_W = 2;
static constexpr u16 EM_X86_64 = 62;
static constexpr u8 ELFCLASS64 = 2;
static constexpr u8 ELFDATA2LSB = 1;
static constexpr u64 PAGE_SIZE = 4096;
static constexpr u64 USER_STACK_SIZE = 4 * PAGE_SIZE;
static constexpr u64 USER_STACK_GUARD_SIZE = PAGE_SIZE;
static constexpr u64 ELF_LINK_BASE = 0x0000008000000000ULL;
static constexpr u64 USER_STACK_OFFSET = 0x01000000;
static constexpr usize MAX_VECTOR = 64;

// Auxiliary vector types from the System V AMD64 ABI.
static constexpr u64 AT_NULL = 0;
static constexpr u64 AT_PAGESZ = 6;
static constexpr u64 AT_BASE = 7;
static constexpr u64 AT_FLAGS = 8;
static constexpr u64 AT_ENTRY = 9;
static constexpr u64 AT_UID = 11;
static constexpr u64 AT_EUID = 12;
static constexpr u64 AT_GID = 13;
static constexpr u64 AT_EGID = 14;
static constexpr u64 AT_SECURE = 23;
static constexpr u64 AT_EXECFN = 31;

struct [[gnu::packed]] ElfHeader {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 program_header_offset;
    u64 section_header_offset;
    u32 flags;
    u16 header_size;
    u16 program_header_size;
    u16 program_header_count;
    u16 section_header_size;
    u16 section_header_count;
    u16 section_name_index;
};

struct [[gnu::packed]] ProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 virtual_address;
    u64 physical_address;
    u64 file_size;
    u64 memory_size;
    u64 alignment;
};

extern "C" const u8 _binary_build_user_demo_elf_start[];
extern "C" const u8 _binary_build_user_demo_elf_end[];
extern "C" const u8 _binary_build_user_kbd_demo_elf_start[];
extern "C" const u8 _binary_build_user_kbd_demo_elf_end[];
extern "C" const u8 _binary_build_user_args_elf_start[];
extern "C" const u8 _binary_build_user_args_elf_end[];

static bool fits(u64 offset, u64 length, u64 total) {
    return offset <= total && length <= total - offset;
}

// Writes the initial process stack. Strings live at the top of the stack; the
// pointer block below them is aligned so that %rsp is 16-byte aligned when the
// task enters Ring 3, as required by the System V AMD64 ABI.
static bool build_initial_stack(address_space::AddressSpace& space,
                                vaddr_t stack_base, vaddr_t stack_top,
                                const char* const* argv, usize argc,
                                const char* const* envp, usize envc,
                                const char* path, u64 entry, u64& stack_pointer) {
    if (argc > MAX_VECTOR || envc > MAX_VECTOR) return false;

    vaddr_t argv_address[MAX_VECTOR];
    vaddr_t envp_address[MAX_VECTOR];
    vaddr_t cursor = stack_top;

    auto push_string = [&](const char* text, vaddr_t& address) -> bool {
        if (!text) return false;
        const u64 length = strlen(text) + 1;
        if (length > cursor - stack_base) return false;
        cursor -= length;
        if (!address_space::copy_to_user(space, cursor, text, length)) return false;
        address = cursor;
        return true;
    };

    for (usize i = 0; i < argc; ++i)
        if (!push_string(argv[i], argv_address[i])) return false;
    for (usize i = 0; i < envc; ++i)
        if (!push_string(envp[i], envp_address[i])) return false;

    // AT_EXECFN points at the file name supplied by the caller.
    vaddr_t execfn = 0;
    if (path && *path) {
        if (!push_string(path, execfn)) return false;
    }

    cursor &= ~static_cast<vaddr_t>(0xF);
    constexpr usize AUX_PAIRS = 11;
    const usize metadata = sizeof(u64)
        * (1 + (argc + 1) + (envc + 1) + 2 * AUX_PAIRS);
    if (metadata > cursor - stack_base) return false;
    vaddr_t rsp = (cursor - metadata) & ~static_cast<vaddr_t>(0xF);
    if (rsp < stack_base || rsp >= stack_top) return false;

    vaddr_t output = rsp;
    auto put = [&](u64 value) -> bool {
        if (!address_space::copy_to_user(space, output, &value, sizeof(value)))
            return false;
        output += sizeof(value);
        return true;
    };

    if (!put(argc)) return false;
    for (usize i = 0; i < argc; ++i)
        if (!put(argv_address[i])) return false;
    if (!put(0)) return false;
    for (usize i = 0; i < envc; ++i)
        if (!put(envp_address[i])) return false;
    if (!put(0)) return false;

    const u64 aux_types[AUX_PAIRS] = {
        AT_PAGESZ, AT_ENTRY, AT_BASE, AT_FLAGS, AT_UID, AT_EUID,
        AT_GID, AT_EGID, AT_SECURE, AT_EXECFN, AT_NULL,
    };
    const u64 aux_values[AUX_PAIRS] = {
        PAGE_SIZE, entry, 0, 0, 0, 0, 0, 0, 0, execfn, 0,
    };
    for (usize i = 0; i < AUX_PAIRS; ++i)
        if (!put(aux_types[i]) || !put(aux_values[i])) return false;

    // The pointer block must not overlap the string area above it.
    if (output > cursor) return false;
    stack_pointer = rsp;
    return true;
}

static bool load_image(const u8* image, usize image_size,
                       address_space::AddressSpace& space,
                       const char* const* argv, usize argc,
                       const char* const* envp, usize envc,
                       const char* path, u64& entry, u64& stack_pointer) {
    if (!image || image_size < sizeof(ElfHeader)) return false;

    const auto* header = reinterpret_cast<const ElfHeader*>(image);
    if (header->ident[0] != 0x7F || header->ident[1] != 'E'
        || header->ident[2] != 'L' || header->ident[3] != 'F'
        || header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB
        || header->ident[6] != 1 || header->type != 2 || header->version != 1
        || header->machine != EM_X86_64 || header->header_size != sizeof(ElfHeader)
        || header->program_header_size != sizeof(ProgramHeader)
        || !fits(header->program_header_offset,
                 static_cast<u64>(header->program_header_count) * sizeof(ProgramHeader),
                 image_size)) return false;

    if (space.user_base < ELF_LINK_BASE || header->entry < ELF_LINK_BASE
        || header->entry > ~static_cast<u64>(0) - (space.user_base - ELF_LINK_BASE))
        return false;
    const u64 load_bias = space.user_base - ELF_LINK_BASE;
    bool loaded_segment = false;
    for (u16 i = 0; i < header->program_header_count; ++i) {
        const auto* program = reinterpret_cast<const ProgramHeader*>(
            image + header->program_header_offset + static_cast<u64>(i) * sizeof(ProgramHeader));
        if (program->type != PT_LOAD || program->memory_size == 0) continue;
        if (program->file_size > program->memory_size || program->virtual_address < ELF_LINK_BASE
            || program->virtual_address > ~static_cast<u64>(0) - load_bias) return false;
        const u64 segment_address = program->virtual_address + load_bias;
        if (segment_address < space.user_base || segment_address >= space.user_limit
            || program->memory_size > space.user_limit - segment_address
            || !fits(program->offset, program->file_size, image_size)) return false;

        const u64 segment_end = segment_address + program->memory_size;
        const u64 page_start = align_down(segment_address, PAGE_SIZE);
        const u64 page_end = align_up(segment_end, PAGE_SIZE);
        const bool writable = (program->flags & PF_W) != 0;
        const bool executable = (program->flags & PF_X) != 0;
        for (u64 page = page_start; page < page_end; page += PAGE_SIZE) {
            if (!address_space::allocate_page(space, page, writable, executable))
                return false;
        }
        if (program->file_size && !address_space::copy_to_user(
                space, segment_address, image + program->offset, program->file_size))
            return false;
        loaded_segment = true;
    }

    entry = header->entry + load_bias;
    if (!loaded_segment || !address_space::contains(space, entry, 1)) return false;
    const u64 stack_top = space.user_base + USER_STACK_OFFSET;
    const u64 stack_base = stack_top - USER_STACK_SIZE;
    // The page just below the stack stays unmapped and acts as a guard: an
    // underflow traps instead of silently corrupting neighbouring mappings.
    const u64 guard_page = stack_base - USER_STACK_GUARD_SIZE;
    if (guard_page < space.user_base) return false;
    for (u64 page = stack_base; page < stack_top; page += PAGE_SIZE) {
        if (!address_space::allocate_page(space, page, true, false)) return false;
    }
    if (!build_initial_stack(space, stack_base, stack_top, argv, argc,
                             envp, envc, path, entry, stack_pointer)) return false;
    return address_space::contains(space, stack_pointer - 1, 1);
}

bool load(address_space::AddressSpace& space, const u8* image, usize image_size,
          const char* const* argv, usize argc,
          const char* const* envp, usize envc,
          const char* path, u64& entry, u64& stack_pointer) {
    return load_image(image, image_size, space, argv, argc, envp, envc,
                      path, entry, stack_pointer);
}

bool load_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer) {
    return load_image(_binary_build_user_demo_elf_start,
                      static_cast<usize>(_binary_build_user_demo_elf_end
                          - _binary_build_user_demo_elf_start),
                      space, nullptr, 0, nullptr, 0, "demo", entry, stack_pointer);
}

bool load_kbd_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer) {
    return load_image(_binary_build_user_kbd_demo_elf_start,
                      static_cast<usize>(_binary_build_user_kbd_demo_elf_end
                          - _binary_build_user_kbd_demo_elf_start),
                      space, nullptr, 0, nullptr, 0, "kbd-demo", entry, stack_pointer);
}

const u8* args_image(usize& size) {
    size = static_cast<usize>(_binary_build_user_args_elf_end
        - _binary_build_user_args_elf_start);
    return _binary_build_user_args_elf_start;
}

} // namespace nord::mm::elf_loader
