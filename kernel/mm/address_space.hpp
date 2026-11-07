#pragma once
#include <nord/types.hpp>

namespace nord::mm::address_space {

struct AddressSpace {
    paddr_t root;
    vaddr_t user_base;
    vaddr_t user_limit;
    u16 pml4_index;
};

static constexpr vaddr_t USER_SLOT_SIZE = 0x0000008000000000ULL;

bool create(AddressSpace& space);
void destroy(AddressSpace& space);
bool map_page(AddressSpace& space, vaddr_t virtual_address, paddr_t physical_address, bool writable);
bool allocate_page(AddressSpace& space, vaddr_t virtual_address, bool writable);
bool copy_to_user(const AddressSpace& space, vaddr_t destination, const void* source, usize size);
bool copy_from_user(const AddressSpace& space, void* destination, vaddr_t source, usize size);
bool contains(const AddressSpace& space, vaddr_t address, usize size);

}