#pragma once
#include <nord/types.hpp>

namespace nord::mm::address_space {

// A demand-paged anonymous mapping created by mmap(). The kernel records the
// permissions here and materializes zero-filled pages on the first fault.
struct UserMapping {
    vaddr_t start;
    vaddr_t end;
    bool writable;
    bool executable;
};

static constexpr usize MAX_USER_MAPPINGS = 32;

struct AddressSpace {
    paddr_t root;
    vaddr_t user_base;
    vaddr_t user_limit;
    u16 pml4_index;

    // Program break ("brk") range. `brk_current` moves with the brk() syscall
    // and pages inside [brk_start, brk_current) are demand-zero.
    vaddr_t brk_start;
    vaddr_t brk_current;
    vaddr_t brk_limit;

    // Anonymous mmap region. It grows upward from `mmap_base`; pages inside
    // [mmap_base, mmap_current) are backed by the mappings below.
    vaddr_t mmap_base;
    vaddr_t mmap_current;
    vaddr_t mmap_limit;
    UserMapping mappings[MAX_USER_MAPPINGS];
    usize mapping_count;
};

static constexpr vaddr_t USER_SLOT_SIZE = 0x0000008000000000ULL;

bool create(AddressSpace& space);
void destroy(AddressSpace& space);
// Records the boot-time kernel PML4 so new address spaces always clone the
// kernel mappings even when create() is called from a Ring 3 syscall.
void set_kernel_root(paddr_t root);
bool map_page(AddressSpace& space, vaddr_t virtual_address, paddr_t physical_address,
              bool writable, bool executable = false);
bool allocate_page(AddressSpace& space, vaddr_t virtual_address,
                   bool writable, bool executable = false);
// Changes the permissions of an existing 4 KiB user mapping.
bool protect_page(AddressSpace& space, vaddr_t virtual_address,
                  bool writable, bool executable);
// Adjusts the program break. A null `requested` address queries the current
// break. Returns false when the new break lies outside [brk_start, brk_limit].
bool brk(AddressSpace& space, vaddr_t requested, vaddr_t& result);
// Creates a demand-paged anonymous mapping of at least `length` bytes.
// `hint` and `flags` are accepted for ABI compatibility; anonymous pages are
// always placed in the private mmap region. `prot` uses the usual PROT_*
// bits (PROT_WRITE = 2, PROT_EXEC = 4).
bool mmap(AddressSpace& space, vaddr_t hint, usize length, u32 prot, u32 flags,
          vaddr_t& result);
// Initializes the program break after the ELF image has been mapped. The
// break starts above the image and the user stack.
bool set_brk_start(AddressSpace& space, vaddr_t image_end);
// Resolves a user page fault by materializing an anonymous page. Returns true
// when the faulting instruction may be retried.
bool handle_fault(AddressSpace& space, vaddr_t address, bool write);
bool copy_to_user(const AddressSpace& space, vaddr_t destination, const void* source, usize size);
bool copy_from_user(const AddressSpace& space, void* destination, vaddr_t source, usize size);
bool contains(const AddressSpace& space, vaddr_t address, usize size);

}
