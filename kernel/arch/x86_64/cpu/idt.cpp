#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/cpu/gdt.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>

namespace nord::arch::idt {

struct [[gnu::packed]] Entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
};

struct [[gnu::packed]] Pointer { u16 limit; u64 base; };

static Entry   g_idt[256];
static Pointer g_pointer;
static Handler g_handlers[256];

}

extern "C" nord::u64 isr_stub_table[256];

namespace nord::arch::idt {

static void set_gate(u8 vector, u64 handler, u8 type_attr, u8 ist = 0) {
    Entry& e = g_idt[vector];
    e.offset_low  = handler & 0xFFFF;
    e.selector    = gdt::KERNEL_CODE;
    e.ist         = ist;
    e.type_attr   = type_attr;
    e.offset_mid  = (handler >> 16) & 0xFFFF;
    e.offset_high = (handler >> 32) & 0xFFFFFFFF;
    e.zero        = 0;
}

void set_handler(u8 vector, Handler fn) { g_handlers[vector] = fn; }
void allow_user(u8 vector) { set_gate(vector, isr_stub_table[vector], 0xEE); }

void init() {
    memset(g_idt, 0, sizeof(g_idt));
    memset(g_handlers, 0, sizeof(g_handlers));

    for (u32 v = 0; v < 256; ++v)
        set_gate(static_cast<u8>(v), isr_stub_table[v], 0x8E);

    set_gate(3, isr_stub_table[3], 0xEE);
    set_gate(8, isr_stub_table[8], 0x8E, gdt::DOUBLE_FAULT_IST);

    g_pointer.limit = sizeof(g_idt) - 1;
    g_pointer.base  = reinterpret_cast<u64>(&g_idt);
    asm volatile("lidt %0" :: "m"(g_pointer));

    kprintf("[idt] installed 256 vectors\n");
}

Handler handler_for(u8 vector) { return g_handlers[vector]; }

}