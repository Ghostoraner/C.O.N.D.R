#pragma once
#include <nord/types.hpp>
#include <nord/limine.hpp>

namespace nord::mm::pmm {

void init(limine::MemmapResponse* memmap, u64 hhdm_offset);

paddr_t alloc_page();
paddr_t alloc_pages(usize count);
void    free_page(paddr_t addr);
void    free_pages(paddr_t addr, usize count);

u64 total_bytes();
u64 used_bytes();
u64 hhdm();

template <typename T> inline T* phys_to_virt(paddr_t p) {
    return reinterpret_cast<T*>(p + hhdm());
}
inline paddr_t virt_to_phys(void* v) {
    return reinterpret_cast<u64>(v) - hhdm();
}

}