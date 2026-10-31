#include <arch/x86_64/cpu/gdt.hpp>
#include <lib/print.hpp>

namespace nord::arch::gdt {

struct [[gnu::packed]] Entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  limit_high_flags;
    u8  base_high;
};

struct [[gnu::packed]] Pointer {
    u16 limit;
    u64 base;
};

static constexpr Entry make(u8 access, u8 flags) {
    return Entry{0, 0, 0, access, static_cast<u8>(flags << 4), 0};
}

static Entry g_gdt[5] = {
    Entry{0, 0, 0, 0, 0, 0},
    make(0x9A, 0xA),
    make(0x92, 0xC),
    make(0xF2, 0xC),
    make(0xFA, 0xA),
};

static Pointer g_pointer;

}

extern "C" void gdt_flush(void* ptr);

namespace nord::arch::gdt {

void init() {
    g_pointer.limit = sizeof(g_gdt) - 1;
    g_pointer.base  = reinterpret_cast<u64>(&g_gdt);
    gdt_flush(&g_pointer);
    kprintf("[gdt] loaded %u descriptors\n",
            static_cast<u32>(sizeof(g_gdt) / sizeof(Entry)));
}

}