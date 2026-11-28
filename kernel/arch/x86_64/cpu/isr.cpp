#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/cpu/pic.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/cpu/syscall.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/print.hpp>
#include <mm/address_space.hpp>

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

extern "C" Registers* isr_dispatch(Registers* r) {
    if (auto fn = idt::handler_for(static_cast<u8>(r->vector))) {
        fn(r);
    }

    if (r->vector >= 32 && r->vector < 48) {
        pic::send_eoi(static_cast<u8>(r->vector - 32));
        return scheduler::schedule(r);
    }
    if (r->vector == 48) return scheduler::schedule(r);
    if (r->vector == 0x80) {
        syscall::dispatch(r);
        return scheduler::schedule(r);
    }
    if (r->vector < 32) {
        if (r->vector == 3 && idt::handler_for(3)) return r;
        // Demand paging: a not-present fault inside a brk/mmap region is
        // resolved by materializing a zero-filled page and retrying the
        // instruction. Protection faults and addresses outside the recorded
        // anonymous regions still terminate the task (or panic in kernel).
        if (r->vector == 14) {
            auto* space = scheduler::current_address_space();
            const u64 address = read_cr2();
            const u64 error = r->error_code;
            if (space && !(error & 1)
                && mm::address_space::handle_fault(*space, address, (error & 2) != 0))
                return r;
        }
        if ((r->cs & 3) == 3) {
            kprintf("[user] task fault vector=%lu rip=%p\n", r->vector, (void*)r->rip);
            scheduler::mark_current_exited(static_cast<i32>(128 + r->vector));
            return scheduler::schedule(r);
        }
        panic_set_frame(r);
        panic("unhandled CPU exception #%lu: %s", r->vector,
              r->vector < 32 ? EXCEPTION_NAMES[r->vector] : "Unknown");
    }
    kprintf("[isr] unexpected interrupt %lu\n", r->vector);
    return r;
}

}