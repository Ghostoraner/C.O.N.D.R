#pragma once
#include <nord/types.hpp>

namespace nord::arch {

static inline void outb(u16 port, u8 val)  { asm volatile("outb %0, %1" :: "a"(val), "Nd"(port)); }
static inline void outw(u16 port, u16 val) { asm volatile("outw %0, %1" :: "a"(val), "Nd"(port)); }
static inline void outl(u16 port, u32 val) { asm volatile("outl %0, %1" :: "a"(val), "Nd"(port)); }

static inline u8  inb(u16 port) { u8  r; asm volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline u16 inw(u16 port) { u16 r; asm volatile("inw %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline u32 inl(u16 port) { u32 r; asm volatile("inl %1, %0" : "=a"(r) : "Nd"(port)); return r; }

static inline void io_wait() { outb(0x80, 0); }

static inline void cli()  { asm volatile("cli"); }
static inline void sti()  { asm volatile("sti"); }
static inline void hlt()  { asm volatile("hlt"); }

[[noreturn]] static inline void halt_forever() {
    for (;;) { asm volatile("cli; hlt"); }
}

static inline u64 read_cr2() { u64 v; asm volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3() { u64 v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(u64 v) { asm volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<u64>(hi) << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 val) {
    asm volatile("wrmsr" :: "c"(msr), "a"(static_cast<u32>(val)), "d"(static_cast<u32>(val >> 32)));
}

}