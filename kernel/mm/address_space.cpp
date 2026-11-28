#include <mm/address_space.hpp>
#include <arch/x86_64/cpu/nx.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/string.hpp>
#include <mm/pmm.hpp>

namespace nord::mm::address_space {

static constexpr u64 PAGE_PRESENT = 1;
static constexpr u64 PAGE_WRITABLE = 2;
static constexpr u64 PAGE_USER = 4;
static constexpr u64 PAGE_LARGE = 0x80;
static constexpr u64 PAGE_NX = 1ULL << 63;
static constexpr u64 PAGE_ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;
static constexpr usize PAGE_SIZE = 4096;

// User layout inside one PML4 slot:
//   [user_base, stack_top)      ELF image and initial stack
//   [stack_top, mmap_base)      brk heap (demand-zero)
//   [mmap_base, mmap_limit)     anonymous mmap region (demand-zero)
static constexpr vaddr_t USER_STACK_TOP_OFFSET = 0x01000000ULL;
static constexpr vaddr_t USER_MMAP_OFFSET = 0x10000000ULL;
static constexpr vaddr_t USER_MMAP_LIMIT_OFFSET = 0x40000000ULL;

static constexpr u32 PROT_WRITE = 2;
static constexpr u32 PROT_EXEC = 4;

static paddr_t g_kernel_root;

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

// Returns a pointer to the leaf PTE when every level is present and marked as
// user-accessible. The pointer remains valid until the page tables change.
static u64* lookup_pte(const AddressSpace& space, vaddr_t address) {
    if (!space.root || address < space.user_base || address >= space.user_limit)
        return nullptr;
    u64* pml4 = table(space.root);
    const u64 e4 = pml4[(address >> 39) & 0x1FF];
    if (!(e4 & PAGE_PRESENT) || (e4 & PAGE_LARGE) || !(e4 & PAGE_USER)) return nullptr;
    u64* pdpt = table(e4 & PAGE_ADDRESS_MASK);
    const u64 e3 = pdpt[(address >> 30) & 0x1FF];
    if (!(e3 & PAGE_PRESENT) || (e3 & PAGE_LARGE) || !(e3 & PAGE_USER)) return nullptr;
    u64* pd = table(e3 & PAGE_ADDRESS_MASK);
    const u64 e2 = pd[(address >> 21) & 0x1FF];
    if (!(e2 & PAGE_PRESENT) || (e2 & PAGE_LARGE) || !(e2 & PAGE_USER)) return nullptr;
    u64* pt = table(e2 & PAGE_ADDRESS_MASK);
    u64* pte = &pt[(address >> 12) & 0x1FF];
    if (!(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER)) return nullptr;
    return pte;
}

static u64 page_entry(const AddressSpace& space, vaddr_t address) {
    const u64* pte = lookup_pte(space, address);
    return pte ? *pte : 0;
}

// Reports whether `page` belongs to a demand-zero region and, if so, what
// permissions a newly materialized page should receive.
static bool anonymous_page(const AddressSpace& space, vaddr_t page,
                           bool& writable, bool& executable) {
    if (page >= space.brk_start && page < space.brk_current) {
        writable = true;
        executable = false;
        return true;
    }
    for (usize i = 0; i < space.mapping_count; ++i) {
        const UserMapping& mapping = space.mappings[i];
        if (page >= mapping.start && page < mapping.end) {
            writable = mapping.writable;
            executable = mapping.executable;
            return true;
        }
    }
    return false;
}

static bool resolve_page(AddressSpace& space, vaddr_t page, bool write) {
    const u64 entry = page_entry(space, page);
    if (entry & PAGE_PRESENT) {
        if (write && !(entry & PAGE_WRITABLE)) return false;
        return true;
    }
    bool writable = false;
    bool executable = false;
    if (!anonymous_page(space, page, writable, executable)) return false;
    if (write && !writable) return false;
    return allocate_page(space, page, writable, executable);
}

static paddr_t unmap_page(AddressSpace& space, vaddr_t address) {
    u64* pte = lookup_pte(space, address);
    if (!pte) return 0;
    const paddr_t physical = *pte & PAGE_ADDRESS_MASK;
    *pte = 0;
    asm volatile("invlpg (%0)" :: "r"(address) : "memory");
    return physical;
}

bool create(AddressSpace& space) {
    space.root = 0;
    space.user_base = 0;
    space.user_limit = 0;
    space.pml4_index = 0;
    space.brk_start = 0;
    space.brk_current = 0;
    space.brk_limit = 0;
    space.mmap_base = 0;
    space.mmap_current = 0;
    space.mmap_limit = 0;
    space.mapping_count = 0;
    memset(space.mappings, 0, sizeof(space.mappings));

    const paddr_t kernel_root = g_kernel_root
        ? g_kernel_root : (arch::read_cr3() & PAGE_ADDRESS_MASK);
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

    space.brk_start = space.user_base + USER_STACK_TOP_OFFSET;
    space.brk_current = space.brk_start;
    space.brk_limit = space.user_base + USER_MMAP_OFFSET;
    space.mmap_base = space.brk_limit;
    space.mmap_current = space.mmap_base;
    space.mmap_limit = space.user_base + USER_MMAP_LIMIT_OFFSET;
    return true;
}

void set_kernel_root(paddr_t root) {
    g_kernel_root = root & PAGE_ADDRESS_MASK;
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
              paddr_t physical_address, bool writable, bool executable) {
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
    const u64 nx = (!executable && arch::nx_enabled()) ? PAGE_NX : 0;
    pt[index] = physical_address | PAGE_PRESENT | PAGE_USER
        | (writable ? PAGE_WRITABLE : 0) | nx;
    return true;
}

bool allocate_page(AddressSpace& space, vaddr_t virtual_address,
                   bool writable, bool executable) {
    const paddr_t physical = pmm::alloc_page();
    if (!physical) return false;
    if (map_page(space, virtual_address, physical, writable, executable)) return true;
    pmm::free_page(physical);
    return false;
}

bool protect_page(AddressSpace& space, vaddr_t virtual_address,
                  bool writable, bool executable) {
    if (!space.root || virtual_address < space.user_base || virtual_address >= space.user_limit
        || (virtual_address & (PAGE_SIZE - 1))) return false;
    u64* pte = lookup_pte(space, virtual_address);
    if (!pte) return false;
    u64 entry = *pte & ~(PAGE_WRITABLE | PAGE_NX);
    if (writable) entry |= PAGE_WRITABLE;
    if (!executable && arch::nx_enabled()) entry |= PAGE_NX;
    *pte = entry;
    asm volatile("invlpg (%0)" :: "r"(virtual_address) : "memory");
    return true;
}

bool set_brk_start(AddressSpace& space, vaddr_t image_end) {
    if (!space.root || image_end > ~static_cast<vaddr_t>(0) - (PAGE_SIZE - 1))
        return false;
    vaddr_t start = align_up(image_end, PAGE_SIZE);
    const vaddr_t minimum = space.user_base + USER_STACK_TOP_OFFSET;
    if (start < minimum) start = minimum;
    if (start > space.mmap_base) return false;
    space.brk_start = start;
    space.brk_current = start;
    space.brk_limit = space.mmap_base;
    return true;
}

bool brk(AddressSpace& space, vaddr_t requested, vaddr_t& result) {
    if (!space.root) return false;
    if (!requested) {
        result = space.brk_current;
        return true;
    }
    if (requested > ~static_cast<vaddr_t>(0) - (PAGE_SIZE - 1)) return false;
    const vaddr_t target = align_up(requested, PAGE_SIZE);
    if (target < space.brk_start || target > space.brk_limit) return false;
    if (target < space.brk_current) {
        for (vaddr_t page = target; page < space.brk_current; page += PAGE_SIZE) {
            const paddr_t physical = unmap_page(space, page);
            if (physical) pmm::free_page(physical);
        }
    }
    space.brk_current = target;
    result = target;
    return true;
}

bool mmap(AddressSpace& space, vaddr_t hint, usize length, u32 prot, u32 flags,
          vaddr_t& result) {
    (void)hint;
    (void)flags;
    if (!space.root || !length) return false;
    if (length > ~static_cast<u64>(0) - (PAGE_SIZE - 1)) return false;
    const u64 aligned = align_up(static_cast<u64>(length), PAGE_SIZE);
    if (space.mmap_current > space.mmap_limit
        || aligned > space.mmap_limit - space.mmap_current) return false;

    const bool writable = (prot & PROT_WRITE) != 0;
    const bool executable = (prot & PROT_EXEC) != 0;
    if (space.mapping_count) {
        UserMapping& last = space.mappings[space.mapping_count - 1];
        if (last.end == space.mmap_current
            && last.writable == writable && last.executable == executable) {
            result = space.mmap_current;
            last.end += aligned;
            space.mmap_current += aligned;
            return true;
        }
    }
    if (space.mapping_count == MAX_USER_MAPPINGS) return false;
    UserMapping& mapping = space.mappings[space.mapping_count++];
    mapping.start = space.mmap_current;
    mapping.end = mapping.start + aligned;
    mapping.writable = writable;
    mapping.executable = executable;
    result = mapping.start;
    space.mmap_current = mapping.end;
    return true;
}

bool handle_fault(AddressSpace& space, vaddr_t address, bool write) {
    if (!space.root || address < space.user_base || address >= space.user_limit)
        return false;
    const vaddr_t page = align_down(address, PAGE_SIZE);
    const u64 entry = page_entry(space, page);
    if (entry & PAGE_PRESENT) {
        if (write && !(entry & PAGE_WRITABLE)) return false;
        return true;
    }
    bool writable = false;
    bool executable = false;
    if (!anonymous_page(space, page, writable, executable)) return false;
    if (write && !writable) return false;
    return allocate_page(space, page, writable, executable);
}

bool contains(const AddressSpace& space, vaddr_t address, usize size) {
    if (!size) return address >= space.user_base && address < space.user_limit;
    if (address < space.user_base || address >= space.user_limit
        || size > space.user_limit - address) return false;
    const vaddr_t end = address + size;
    for (vaddr_t page = address & ~(static_cast<vaddr_t>(PAGE_SIZE) - 1); page < end; page += PAGE_SIZE) {
        if (page_entry(space, page) & PAGE_PRESENT) continue;
        bool writable = false;
        bool executable = false;
        if (!anonymous_page(space, page, writable, executable)) return false;
    }
    return true;
}

bool copy_to_user(const AddressSpace& space, vaddr_t destination,
                  const void* source, usize size) {
    if (!source) return size == 0;
    if (!contains(space, destination, size)) return false;
    AddressSpace& mutable_space = const_cast<AddressSpace&>(space);
    const auto* input = static_cast<const u8*>(source);
    while (size) {
        const vaddr_t page_address = destination & ~(static_cast<vaddr_t>(PAGE_SIZE) - 1);
        if (!resolve_page(mutable_space, page_address, true)) return false;
        const u64 entry = page_entry(space, page_address);
        if (!(entry & PAGE_PRESENT) || !(entry & PAGE_WRITABLE)) return false;
        const usize offset = destination & (PAGE_SIZE - 1);
        usize chunk = PAGE_SIZE - offset;
        if (chunk > size) chunk = size;
        auto* page = static_cast<u8*>(pmm::phys_to_virt<void>(
            (entry & PAGE_ADDRESS_MASK) + offset));
        memcpy(page, input, chunk);
        input += chunk;
        destination += chunk;
        size -= chunk;
    }
    return true;
}

bool copy_from_user(const AddressSpace& space, void* destination,
                    vaddr_t source, usize size) {
    if (!destination) return size == 0;
    if (!contains(space, source, size)) return false;
    AddressSpace& mutable_space = const_cast<AddressSpace&>(space);
    auto* output = static_cast<u8*>(destination);
    while (size) {
        const vaddr_t page_address = source & ~(static_cast<vaddr_t>(PAGE_SIZE) - 1);
        if (!resolve_page(mutable_space, page_address, false)) return false;
        const u64 entry = page_entry(space, page_address);
        if (!(entry & PAGE_PRESENT)) return false;
        const usize offset = source & (PAGE_SIZE - 1);
        usize chunk = PAGE_SIZE - offset;
        if (chunk > size) chunk = size;
        auto* page = static_cast<const u8*>(pmm::phys_to_virt<void>(
            (entry & PAGE_ADDRESS_MASK) + offset));
        memcpy(output, page, chunk);
        output += chunk;
        source += chunk;
        size -= chunk;
    }
    return true;
}

}
