#include <mm/pmm.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>

namespace nord::mm::pmm {

static u8*   g_bitmap      = nullptr;
static u64   g_bitmap_size = 0;
static u64   g_total_pages = 0;
static u64   g_used_pages  = 0;
static u64   g_hhdm        = 0;
static u64   g_last_hint   = 0;

static inline void bit_set(u64 i)   { g_bitmap[i / 8] |=  (1u << (i % 8)); }
static inline void bit_clear(u64 i) { g_bitmap[i / 8] &= ~(1u << (i % 8)); }
static inline bool bit_test(u64 i)  { return g_bitmap[i / 8] & (1u << (i % 8)); }

u64 hhdm()        { return g_hhdm; }
u64 total_bytes() { return g_total_pages * PAGE_SIZE; }
u64 used_bytes()  { return g_used_pages  * PAGE_SIZE; }

void init(limine::MemmapResponse* memmap, u64 hhdm_offset) {
    g_hhdm = hhdm_offset;

    u64 highest = 0;
    for (u64 i = 0; i < memmap->entry_count; ++i) {
        auto* e = memmap->entries[i];
        if (e->type != limine::MEMMAP_USABLE) continue;
        u64 top = e->base + e->length;
        if (top > highest) highest = top;
    }
    g_total_pages = highest / PAGE_SIZE;
    g_bitmap_size = align_up(g_total_pages / 8, PAGE_SIZE);

    for (u64 i = 0; i < memmap->entry_count; ++i) {
        auto* e = memmap->entries[i];
        if (e->type != limine::MEMMAP_USABLE || e->length < g_bitmap_size) continue;
        g_bitmap = reinterpret_cast<u8*>(e->base + g_hhdm);
        e->base   += g_bitmap_size;
        e->length -= g_bitmap_size;
        break;
    }
    if (!g_bitmap) panic("pmm: not enough memory for bitmap (%lu bytes)", g_bitmap_size);

    memset(g_bitmap, 0xFF, g_bitmap_size);
    g_used_pages = g_total_pages;

    for (u64 i = 0; i < memmap->entry_count; ++i) {
        auto* e = memmap->entries[i];
        if (e->type != limine::MEMMAP_USABLE) continue;
        for (u64 off = 0; off < e->length; off += PAGE_SIZE) {
            u64 idx = (e->base + off) / PAGE_SIZE;
            if (idx >= g_total_pages) break;
            bit_clear(idx);
            --g_used_pages;
        }
    }

    if (!bit_test(0)) { bit_set(0); ++g_used_pages; }

    kprintf("[pmm] %lu MiB total, %lu MiB free, bitmap %lu KiB @ %p\n",
            total_bytes() / (1024 * 1024),
            (total_bytes() - used_bytes()) / (1024 * 1024),
            g_bitmap_size / 1024, g_bitmap);
}

static paddr_t find_range(usize count, u64 start, u64 end) {
    u64 run = 0;
    for (u64 i = start; i < end; ++i) {
        if (bit_test(i)) { run = 0; continue; }
        if (++run == count) {
            u64 first = i + 1 - count;
            for (u64 j = first; j <= i; ++j) bit_set(j);
            g_used_pages += count;
            g_last_hint = i + 1;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

paddr_t alloc_pages(usize count) {
    if (count == 0) return 0;
    paddr_t p = find_range(count, g_last_hint, g_total_pages);
    if (!p) { g_last_hint = 0; p = find_range(count, 0, g_total_pages); }
    if (p) memset(phys_to_virt<void>(p), 0, count * PAGE_SIZE);
    return p;
}

paddr_t alloc_page() { return alloc_pages(1); }

void free_pages(paddr_t addr, usize count) {
    u64 idx = addr / PAGE_SIZE;
    for (usize i = 0; i < count; ++i) {
        if (!bit_test(idx + i)) panic("pmm: double free of page %p", (void*)addr);
        bit_clear(idx + i);
        --g_used_pages;
    }
    if (idx < g_last_hint) g_last_hint = idx;
}

void free_page(paddr_t addr) { free_pages(addr, 1); }

}