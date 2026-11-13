#include <arch/x86_64/cpu/gdt.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>

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

struct [[gnu::packed]] TaskStateSegment {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

struct [[gnu::packed]] TssDescriptor {
    u16 limit_low;
    u16 base_low;
    u8 base_mid;
    u8 access;
    u8 limit_high_flags;
    u8 base_mid2;
    u32 base_high;
    u32 reserved;
};

static constexpr Entry make(u8 access, u8 flags) {
    return Entry{0, 0, 0, access, static_cast<u8>(flags << 4), 0};
}

static Entry g_gdt[7] = {
    Entry{0, 0, 0, 0, 0, 0},
    make(0x9A, 0xA),
    make(0x92, 0xC),
    make(0xF2, 0xC),
    make(0xFA, 0xA),
};

static Pointer g_pointer;
static TaskStateSegment g_tss;

static constexpr usize DOUBLE_FAULT_STACK_SIZE = 16 * 1024;
alignas(16) static u8 g_double_fault_stack[DOUBLE_FAULT_STACK_SIZE];

}

extern "C" void gdt_flush(void* ptr);

namespace nord::arch::gdt {

void init() {
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.iomap_base = sizeof(g_tss);
    g_tss.ist1 = reinterpret_cast<u64>(g_double_fault_stack) + DOUBLE_FAULT_STACK_SIZE;
    TssDescriptor descriptor{};
    const u64 base = reinterpret_cast<u64>(&g_tss);
    const u32 limit = sizeof(g_tss) - 1;
    descriptor.limit_low = limit & 0xFFFF;
    descriptor.base_low = base & 0xFFFF;
    descriptor.base_mid = (base >> 16) & 0xFF;
    descriptor.access = 0x89;
    descriptor.limit_high_flags = (limit >> 16) & 0x0F;
    descriptor.base_mid2 = (base >> 24) & 0xFF;
    descriptor.base_high = base >> 32;
    memcpy(&g_gdt[5], &descriptor, sizeof(descriptor));

    g_pointer.limit = sizeof(g_gdt) - 1;
    g_pointer.base  = reinterpret_cast<u64>(&g_gdt);
    gdt_flush(&g_pointer);
    u16 selector = TSS_SELECTOR;
    asm volatile("ltr %0" :: "r"(selector));
    kprintf("[gdt] loaded %u descriptors\n",
            static_cast<u32>(sizeof(g_gdt) / sizeof(Entry)));
}

void set_kernel_stack(u64 stack_top) {
    g_tss.rsp0 = stack_top;
}

}