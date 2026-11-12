#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>

namespace nord::mm::heap {

struct PreemptionGuard {
    PreemptionGuard() { arch::scheduler::preempt_disable(); }
    ~PreemptionGuard() { arch::scheduler::preempt_enable(); }
};

struct Block {
    u64    size;
    bool   free;
    Block* next;
    Block* prev;
};

static constexpr u64 HEADER   = sizeof(Block);
static constexpr u64 MIN_SPLIT = HEADER + 32;
static constexpr u64 GROW_PAGES = 64;

static Block* g_head = nullptr;
static u64    g_capacity = 0;
static u64    g_used = 0;

static Block* grow(u64 need) {
    u64 pages = align_up(need + HEADER, PAGE_SIZE) / PAGE_SIZE;
    if (pages < GROW_PAGES) pages = GROW_PAGES;

    paddr_t phys = pmm::alloc_pages(pages);
    if (!phys) return nullptr;

    auto* b = pmm::phys_to_virt<Block>(phys);
    b->size = pages * PAGE_SIZE - HEADER;
    b->free = true;
    b->next = nullptr;
    b->prev = nullptr;

    if (!g_head) { g_head = b; }
    else {
        Block* tail = g_head;
        while (tail->next) tail = tail->next;
        tail->next = b;
        b->prev = tail;
    }
    g_capacity += pages * PAGE_SIZE;
    return b;
}

void init() {
    PreemptionGuard guard;
    if (!grow(GROW_PAGES * PAGE_SIZE)) panic("heap: failed to allocate initial pool");
    kprintf("[heap] initialized, %lu KiB\n", g_capacity / 1024);
}

static void split(Block* b, u64 size) {
    if (b->size < size + MIN_SPLIT) return;
    auto* rest = reinterpret_cast<Block*>(reinterpret_cast<u8*>(b) + HEADER + size);
    rest->size = b->size - size - HEADER;
    rest->free = true;
    rest->next = b->next;
    rest->prev = b;
    if (b->next) b->next->prev = rest;
    b->next = rest;
    b->size = size;
}

void* kmalloc(usize size) {
    PreemptionGuard guard;
    if (size == 0) return nullptr;
    size = align_up(size, 16);

    for (Block* b = g_head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        split(b, size);
        b->free = false;
        g_used += b->size;
        return reinterpret_cast<u8*>(b) + HEADER;
    }

    Block* b = grow(size);
    if (!b) return nullptr;
    split(b, size);
    b->free = false;
    g_used += b->size;
    return reinterpret_cast<u8*>(b) + HEADER;
}

void* kcalloc(usize count, usize size) {
    if (count && size > ~static_cast<usize>(0) / count) return nullptr;
    const usize total = count * size;
    void* p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void coalesce(Block* b) {
    auto adjacent = [](Block* a, Block* c) {
        return reinterpret_cast<u8*>(a) + HEADER + a->size == reinterpret_cast<u8*>(c);
    };
    if (b->next && b->next->free && adjacent(b, b->next)) {
        Block* n = b->next;
        b->size += HEADER + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && b->prev->free && adjacent(b->prev, b)) {
        Block* p = b->prev;
        p->size += HEADER + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
    }
}

void kfree(void* ptr) {
    PreemptionGuard guard;
    if (!ptr) return;
    auto* b = reinterpret_cast<Block*>(static_cast<u8*>(ptr) - HEADER);
    if (b->free) panic("heap: double free at %p", ptr);
    b->free = true;
    g_used -= b->size;
    coalesce(b);
}

void* krealloc(void* ptr, usize size) {
    PreemptionGuard guard;
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return nullptr; }

    auto* b = reinterpret_cast<Block*>(static_cast<u8*>(ptr) - HEADER);
    if (b->size >= size) return ptr;

    void* np = kmalloc(size);
    if (!np) return nullptr;
    memcpy(np, ptr, b->size);
    kfree(ptr);
    return np;
}

void stats(u64& used, u64& capacity) {
    PreemptionGuard guard;
    used = g_used;
    capacity = g_capacity;
}

}

void* operator new(nord::usize n)              { return nord::mm::heap::kmalloc(n); }
void* operator new[](nord::usize n)            { return nord::mm::heap::kmalloc(n); }
void  operator delete(void* p) noexcept        { nord::mm::heap::kfree(p); }
void  operator delete[](void* p) noexcept      { nord::mm::heap::kfree(p); }
void  operator delete(void* p, nord::usize) noexcept   { nord::mm::heap::kfree(p); }
void  operator delete[](void* p, nord::usize) noexcept { nord::mm::heap::kfree(p); }
void* operator new(nord::usize, void* p) noexcept   { return p; }
void* operator new[](nord::usize, void* p) noexcept { return p; }