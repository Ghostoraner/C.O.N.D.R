#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/cpu/pic.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/print.hpp>

namespace nord::arch {

namespace idt { idt::Handler handler_for(u8 vector); }

static const char* EXCEPTION_NAMES[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved",
};

static void dump(Registers* r) {
    kprintf("\nException #%lu: %s\n", r->vector,
            r->vector < 32 ? EXCEPTION_NAMES[r->vector] : "Unknown");
    kprintf("  error_code = %#lx\n", r->error_code);
    kprintf("  RIP = %p   CS  = %#lx  RFLAGS = %#lx\n", (void*)r->rip, r->cs, r->rflags);
    kprintf("  RSP = %p   SS  = %#lx\n", (void*)r->rsp, r->ss);
    kprintf("  RAX = %p   RBX = %p\n", (void*)r->rax, (void*)r->rbx);
    kprintf("  RCX = %p   RDX = %p\n", (void*)r->rcx, (void*)r->rdx);
    kprintf("  RSI = %p   RDI = %p\n", (void*)r->rsi, (void*)r->rdi);
    kprintf("  RBP = %p   CR2 = %p\n", (void*)r->rbp, (void*)read_cr2());

    if (r->vector == 14) {
        u64 ec = r->error_code;
        kprintf("  Page fault: %s, %s, %s%s\n",
                (ec & 1) ? "protection violation" : "page not present",
                (ec & 2) ? "write" : "read",
                (ec & 4) ? "ring 3" : "ring 0",
                (ec & 16) ? ", instruction fetch" : "");
    }
}

extern "C" void isr_dispatch(Registers* r) {
    if (auto fn = idt::handler_for(static_cast<u8>(r->vector))) {
        fn(r);
        if (r->vector < 32) return;
    }

    if (r->vector >= 32 && r->vector < 48) {
        pic::send_eoi(static_cast<u8>(r->vector - 32));
        return;
    }
    if (r->vector < 32) {
        dump(r);
        panic("unhandled CPU exception");
    }
    kprintf("[isr] unexpected interrupt %lu\n", r->vector);
}

}