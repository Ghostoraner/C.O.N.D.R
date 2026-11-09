#include <mm/elf_loader.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <nord/types.hpp>

namespace nord::mm::elf_loader {

static constexpr u32 PT_LOAD = 1;
static constexpr u32 PF_W = 2;
static constexpr u16 EM_X86_64 = 62;
static constexpr u8 ELFCLASS64 = 2;
static constexpr u8 ELFDATA2LSB = 1;
static constexpr u64 PAGE_SIZE = 4096;
static constexpr u64 USER_STACK_SIZE = PAGE_SIZE;
static constexpr u64 ELF_LINK_BASE = 0x0000008000000000ULL;
static constexpr u64 USER_STACK_OFFSET = 0x01000000;

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

static bool fits(u64 offset, u64 length, u64 total) {
    return offset <= total && length <= total - offset;
}

namespace {
bool load_image(const u8* image, const u8* image_end,
                address_space::AddressSpace& space, u64& entry, u64& stack_pointer) {
    const u64 image_size = image_end - image;
    if (image_size < sizeof(ElfHeader)) return false;

    const auto* header = reinterpret_cast<const ElfHeader*>(image);
    if (header->ident[0] != 0x7F || header->ident[1] != 'E'
        || header->ident[2] != 'L' || header->ident[3] != 'F'
        || header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB
        || header->ident[6] != 1 || header->type != 2 || header->version != 1
        || header->machine != EM_X86_64 || header->header_size != sizeof(ElfHeader)
        || header->program_header_size != sizeof(ProgramHeader)
        || !fits(header->program_header_offset,
                 static_cast<u64>(header->program_header_count) * sizeof(ProgramHeader), image_size)) return false;

    if (space.user_base < ELF_LINK_BASE || header->entry < ELF_LINK_BASE
        || header->entry > ~static_cast<u64>(0) - (space.user_base - ELF_LINK_BASE)) return false;
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
        for (u64 page = page_start; page < page_end; page += PAGE_SIZE) {
            if (!address_space::allocate_page(space, page, (program->flags & PF_W) != 0))
                return false;
        }
        if (program->file_size && !address_space::copy_to_user(
                space, segment_address, image + program->offset, program->file_size)) return false;
        loaded_segment = true;
    }

    entry = header->entry + load_bias;
    if (!loaded_segment || !address_space::contains(space, entry, 1)) return false;
    const u64 stack_top = space.user_base + USER_STACK_OFFSET;
    const u64 stack_base = stack_top - USER_STACK_SIZE;
    if (!address_space::allocate_page(space, stack_base, true)) return false;
    stack_pointer = stack_top;
    return true;
}
} // namespace

bool load_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer) {
    return load_image(_binary_build_user_demo_elf_start, _binary_build_user_demo_elf_end,
                      space, entry, stack_pointer);
}

bool load_kbd_demo(address_space::AddressSpace& space, u64& entry, u64& stack_pointer) {
    return load_image(_binary_build_user_kbd_demo_elf_start, _binary_build_user_kbd_demo_elf_end,
                      space, entry, stack_pointer);
}

} // namespace nord::mm::elf_loader