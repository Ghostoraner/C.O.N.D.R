#include <mm/task_stack.hpp>
#include <arch/x86_64/cpu/nx.hpp>
#include <arch/x86_64/io.hpp>
#include <mm/pmm.hpp>

namespace nord::mm::task_stack {

static constexpr u64 PAGE_PRESENT = 1;
static constexpr u64 PAGE_WRITABLE = 2;
static constexpr u64 PAGE_LARGE = 0x80;
static constexpr u64 PAGE_NX = 1ULL << 63;
static constexpr u64 PAGE_ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;
static constexpr usize MAX_STACKS = 16;

static paddr_t g_root;
static vaddr_t g_base;
static usize g_stride;
static usize g_stack_size;
static bool g_slots[MAX_STACKS];

static u64* table(paddr_t physical) {
    return pmm::phys_to_virt<u64>(physical);
}

static u64* next_table(u64* parent, usize index) {
    const u64 entry = parent[index];
    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_LARGE) return nullptr;
        return table(entry & PAGE_ADDRESS_MASK);
    }
    const paddr_t child = pmm::alloc_page();
    if (!child) return nullptr;
    parent[index] = child | PAGE_PRESENT | PAGE_WRITABLE;
    return table(child);
}

static bool map_page(vaddr_t address, paddr_t physical, bool executable) {
    u64* pml4 = table(g_root);
    u64* pdpt = next_table(pml4, (address >> 39) & 0x1FF);
    if (!pdpt) return false;
    u64* pd = next_table(pdpt, (address >> 30) & 0x1FF);
    if (!pd) return false;
    u64* pt = next_table(pd, (address >> 21) & 0x1FF);
    if (!pt) return false;
    const usize index = (address >> 12) & 0x1FF;
    if (pt[index] & PAGE_PRESENT) return false;
    const u64 nx = (!executable && arch::nx_enabled()) ? PAGE_NX : 0;
    pt[index] = physical | PAGE_PRESENT | PAGE_WRITABLE | nx;
    return true;
}

static paddr_t unmap_page(vaddr_t address) {
    u64* pml4 = table(g_root);
    const u64 e4 = pml4[(address >> 39) & 0x1FF];
    if (!(e4 & PAGE_PRESENT) || (e4 & PAGE_LARGE)) return 0;
    u64* pdpt = table(e4 & PAGE_ADDRESS_MASK);
    const u64 e3 = pdpt[(address >> 30) & 0x1FF];
    if (!(e3 & PAGE_PRESENT) || (e3 & PAGE_LARGE)) return 0;
    u64* pd = table(e3 & PAGE_ADDRESS_MASK);
    const u64 e2 = pd[(address >> 21) & 0x1FF];
    if (!(e2 & PAGE_PRESENT) || (e2 & PAGE_LARGE)) return 0;
    u64* pt = table(e2 & PAGE_ADDRESS_MASK);
    const usize index = (address >> 12) & 0x1FF;
    const u64 entry = pt[index];
    if (!(entry & PAGE_PRESENT)) return 0;
    pt[index] = 0;
    asm volatile("invlpg (%0)" :: "r"(address) : "memory");
    return entry & PAGE_ADDRESS_MASK;
}

bool init(usize stack_size) {
    if (g_base) return true;
    g_root = arch::read_cr3() & PAGE_ADDRESS_MASK;
    if (!g_root || !stack_size) return false;
    g_stack_size = align_up(stack_size, PAGE_SIZE);
    g_stride = g_stack_size + PAGE_SIZE;

    u64* pml4 = table(g_root);
    for (usize index = 510; index > 256; --index) {
        if (pml4[index] & PAGE_PRESENT) continue;
        u64 base = static_cast<u64>(index) << 39;
        if (index & 0x100) base |= 0xFFFF000000000000ULL;

        // Reserve the PML4 slot now, not on the first allocation. Address
        // spaces created later copy this entry, so the stack region must
        // already be visible in the shared kernel half.
        const paddr_t pdpt = pmm::alloc_page();
        if (!pdpt) return false;
        pml4[index] = pdpt | PAGE_PRESENT | PAGE_WRITABLE;
        asm volatile("invlpg (%0)" :: "r"(base) : "memory");

        g_base = base;
        for (usize i = 0; i < MAX_STACKS; ++i) g_slots[i] = false;
        return true;
    }
    return false;
}

void* allocate() {
    if (!g_base) return nullptr;
    usize slot = 0;
    while (slot < MAX_STACKS && g_slots[slot]) ++slot;
    if (slot == MAX_STACKS) return nullptr;

    const vaddr_t base = g_base + static_cast<vaddr_t>(slot) * g_stride + PAGE_SIZE;
    usize mapped = 0;
    for (; mapped < g_stack_size; mapped += PAGE_SIZE) {
        const paddr_t physical = pmm::alloc_page();
        if (!physical || !map_page(base + mapped, physical, false)) {
            if (physical) pmm::free_page(physical);
            break;
        }
    }
    if (mapped != g_stack_size) {
        for (usize offset = 0; offset < mapped; offset += PAGE_SIZE) {
            const paddr_t physical = unmap_page(base + offset);
            if (physical) pmm::free_page(physical);
        }
        return nullptr;
    }

    g_slots[slot] = true;
    return reinterpret_cast<void*>(base);
}

u64 top(const void* base) {
    return reinterpret_cast<u64>(base) + g_stack_size;
}

void release(void* base) {
    if (!base || !g_base) return;
    const vaddr_t address = reinterpret_cast<vaddr_t>(base);
    for (usize offset = 0; offset < g_stack_size; offset += PAGE_SIZE) {
        const paddr_t physical = unmap_page(address + offset);
        if (physical) pmm::free_page(physical);
    }
    const usize slot = (address - g_base - PAGE_SIZE) / g_stride;
    if (slot < MAX_STACKS) g_slots[slot] = false;
}

}
