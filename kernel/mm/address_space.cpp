#include <mm/address_space.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/string.hpp>
#include <mm/pmm.hpp>

namespace nord::mm::address_space {

static constexpr u64 PAGE_PRESENT = 1;
static constexpr u64 PAGE_WRITABLE = 2;
static constexpr u64 PAGE_USER = 4;
static constexpr u64 PAGE_LARGE = 0x80;
static constexpr u64 PAGE_ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;
static constexpr usize PAGE_SIZE = 4096;
static u64* table(paddr_t physical) {
    return pmm::phys_to_virt<u64>(physical);
}

static bool ensure_table(u64* parent, usize index, u64& entry) {
    entry = parent[index];
    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_LARGE) return false;
        entry |= PAGE_WRITABLE | PAGE_USER;
        parent[index] = entry;
        return true;
    }
    const paddr_t child = pmm::alloc_page();
    if (!child) return false;
    entry = child | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    parent[index] = entry;
    return true;
}

static paddr_t translate(const AddressSpace& space, vaddr_t address) {
    if (address < space.user_base || address >= space.user_limit || !space.root) return 0;
    u64* pml4 = table(space.root);
    const u64 e4 = pml4[(address >> 39) & 0x1FF];
    if (!(e4 & PAGE_PRESENT) || (e4 & PAGE_USER) == 0) return 0;
    u64* pdpt = table(e4 & PAGE_ADDRESS_MASK);
    const u64 e3 = pdpt[(address >> 30) & 0x1FF];
    if (!(e3 & PAGE_PRESENT) || (e3 & PAGE_LARGE) || (e3 & PAGE_USER) == 0) return 0;
    u64* pd = table(e3 & PAGE_ADDRESS_MASK);
    const u64 e2 = pd[(address >> 21) & 0x1FF];
    if (!(e2 & PAGE_PRESENT) || (e2 & PAGE_LARGE) || (e2 & PAGE_USER) == 0) return 0;
    u64* pt = table(e2 & PAGE_ADDRESS_MASK);
    const u64 e1 = pt[(address >> 12) & 0x1FF];
    if (!(e1 & PAGE_PRESENT) || (e1 & PAGE_USER) == 0) return 0;
    return (e1 & PAGE_ADDRESS_MASK) | (address & (PAGE_SIZE - 1));
}

bool create(AddressSpace& space) {
    space.root = 0;
    space.user_base = 0;
    space.user_limit = 0;
    space.pml4_index = 0;
    const paddr_t kernel_root = arch::read_cr3() & PAGE_ADDRESS_MASK;
    if (!kernel_root) return false;
    const u64* source = table(kernel_root);
    usize free_slot = 1;
    while (free_slot < 256 && (source[free_slot] & PAGE_PRESENT)) ++free_slot;
    if (free_slot == 256) return false;

    const paddr_t root = pmm::alloc_page();
    if (!root) return false;
    memcpy(table(root), source, PAGE_SIZE);
    space.root = root;
    space.pml4_index = static_cast<u16>(free_slot);
    space.user_base = static_cast<vaddr_t>(free_slot) * USER_SLOT_SIZE;
    space.user_limit = space.user_base + USER_SLOT_SIZE;
    return true;
}

void destroy(AddressSpace& space) {
    if (!space.root) return;
    u64* pml4 = table(space.root);
    const u64 e4 = pml4[space.pml4_index];
    if (e4 & PAGE_PRESENT) {
        const paddr_t pdpt_phys = e4 & PAGE_ADDRESS_MASK;
        u64* pdpt = table(pdpt_phys);
        for (usize i3 = 0; i3 < 512; ++i3) {
            const u64 e3 = pdpt[i3];
            if (!(e3 & PAGE_PRESENT) || (e3 & PAGE_LARGE)) continue;
            const paddr_t pd_phys = e3 & PAGE_ADDRESS_MASK;
            u64* pd = table(pd_phys);
            for (usize i2 = 0; i2 < 512; ++i2) {
                const u64 e2 = pd[i2];
                if (!(e2 & PAGE_PRESENT) || (e2 & PAGE_LARGE)) continue;
                const paddr_t pt_phys = e2 & PAGE_ADDRESS_MASK;
                u64* pt = table(pt_phys);
                for (usize i1 = 0; i1 < 512; ++i1) {
                    const u64 e1 = pt[i1];
                    if (e1 & PAGE_PRESENT) pmm::free_page(e1 & PAGE_ADDRESS_MASK);
                }
                pmm::free_page(pt_phys);
            }
            pmm::free_page(pd_phys);
        }
        pmm::free_page(pdpt_phys);
    }
    pmm::free_page(space.root);
    space.root = 0;
}

bool map_page(AddressSpace& space, vaddr_t virtual_address,
              paddr_t physical_address, bool writable) {
    if (!space.root || virtual_address < space.user_base || virtual_address >= space.user_limit
        || (virtual_address & (PAGE_SIZE - 1)) || (physical_address & (PAGE_SIZE - 1))) return false;

    u64* pml4 = table(space.root);
    u64 e4;
    if (!ensure_table(pml4, (virtual_address >> 39) & 0x1FF, e4)) return false;
    u64* pdpt = table(e4 & PAGE_ADDRESS_MASK);
    u64 e3;
    if (!ensure_table(pdpt, (virtual_address >> 30) & 0x1FF, e3)) return false;
    u64* pd = table(e3 & PAGE_ADDRESS_MASK);
    u64 e2;
    if (!ensure_table(pd, (virtual_address >> 21) & 0x1FF, e2)) return false;
    u64* pt = table(e2 & PAGE_ADDRESS_MASK);
    const usize index = (virtual_address >> 12) & 0x1FF;
    if (pt[index] & PAGE_PRESENT) return false;
    pt[index] = physical_address | PAGE_PRESENT | PAGE_USER | (writable ? PAGE_WRITABLE : 0);
    return true;
}

bool allocate_page(AddressSpace& space, vaddr_t virtual_address, bool writable) {
    const paddr_t physical = pmm::alloc_page();
    if (!physical) return false;
    if (map_page(space, virtual_address, physical, writable)) return true;
    pmm::free_page(physical);
    return false;
}

bool contains(const AddressSpace& space, vaddr_t address, usize size) {
    if (!size) return address >= space.user_base && address < space.user_limit;
    if (address < space.user_base || address >= space.user_limit
        || size > space.user_limit - address) return false;
    const vaddr_t end = address + size;
    for (vaddr_t page = address & ~(static_cast<vaddr_t>(PAGE_SIZE) - 1); page < end; page += PAGE_SIZE)
        if (!translate(space, page)) return false;
    return true;
}

bool copy_to_user(const AddressSpace& space, vaddr_t destination,
                  const void* source, usize size) {
    if (!source || !contains(space, destination, size)) return size == 0;
    const auto* input = static_cast<const u8*>(source);
    while (size) {
        const paddr_t physical = translate(space, destination);
        const usize offset = destination & (PAGE_SIZE - 1);
        usize chunk = PAGE_SIZE - offset;
        if (chunk > size) chunk = size;
        auto* page = static_cast<u8*>(pmm::phys_to_virt<void>(physical & PAGE_ADDRESS_MASK));
        memcpy(page + offset, input, chunk);
        input += chunk;
        destination += chunk;
        size -= chunk;
    }
    return true;
}

bool copy_from_user(const AddressSpace& space, void* destination,
                    vaddr_t source, usize size) {
    if (!destination || !contains(space, source, size)) return size == 0;
    auto* output = static_cast<u8*>(destination);
    while (size) {
        const paddr_t physical = translate(space, source);
        const usize offset = source & (PAGE_SIZE - 1);
        usize chunk = PAGE_SIZE - offset;
        if (chunk > size) chunk = size;
        auto* page = static_cast<const u8*>(pmm::phys_to_virt<void>(physical & PAGE_ADDRESS_MASK));
        memcpy(output, page + offset, chunk);
        output += chunk;
        source += chunk;
        size -= chunk;
    }
    return true;
}

}