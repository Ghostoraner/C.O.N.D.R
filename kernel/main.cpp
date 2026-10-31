#include <nord/types.hpp>
#include <nord/limine.hpp>
#include <arch/x86_64/io.hpp>
#include <arch/x86_64/cpu/gdt.hpp>
#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/cpu/pic.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/dev/serial.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/dev/timer.hpp>
#include <drivers/video/framebuffer.hpp>
#include <mm/pmm.hpp>
#include <mm/heap.hpp>
#include <lib/print.hpp>
#include <shell/custom_shell.hpp>
#include <fs/nvmfs.hpp>

using namespace nord;

static u64 g_worker_iterations;

static void shell_task() {
    shell::custom_shell_run();
}

static void worker_task() {
    for (;;) {
        __atomic_add_fetch(&g_worker_iterations, 1, __ATOMIC_RELAXED);
        arch::scheduler::yield();
    }
}

namespace {
LIMINE_REQ volatile limine::MemmapRequest        g_memmap  = { LIMINE_MEMMAP_REQUEST, 0, nullptr };
LIMINE_REQ volatile limine::HhdmRequest          g_hhdm    = { LIMINE_HHDM_REQUEST, 0, nullptr };
LIMINE_REQ volatile limine::FramebufferRequest   g_fb      = { LIMINE_FRAMEBUFFER_REQUEST, 0, nullptr };
LIMINE_REQ volatile limine::KernelAddressRequest g_kaddr   = { LIMINE_KERNEL_ADDRESS_REQUEST, 0, nullptr };
LIMINE_REQ volatile limine::RsdpRequest          g_rsdp    = { LIMINE_RSDP_REQUEST, 0, nullptr };
}

static const char* BANNER =
    "\n"
    "   ____   ___   _   _  ____  \n"
    "  / ___| / _ \\ | \\ | ||  _ \\ \n"
    " | |    | | | ||  \\| || | | |\n"
    " | |___ | |_| || |\\  || |_| |\n"
    "  \\____| \\___/ |_| \\_||____/ \n"
    "\n C.O.N.D.R. x86_64 kernel booting...\n\n";

static void test_subsystems() {
    void* a = mm::heap::kmalloc(128);
    void* b = mm::heap::kmalloc(4096);
    kprintf("[test] kmalloc(128) = %p, kmalloc(4096) = %p\n", a, b);
    mm::heap::kfree(a);
    mm::heap::kfree(b);

    u64 used, cap;
    mm::heap::stats(used, cap);
    kprintf("[test] heap: %lu of %lu bytes used\n", used, cap);

    kprintf("[test] triggering int3...\n");
    asm volatile("int3");
    kprintf("[test] returned from handler; IDT is working\n");
}

static void on_breakpoint(arch::Registers* r) {
    kprintf("[isr] breakpoint @ %p\n", (void*)r->rip);
}

extern "C" [[noreturn]] void kernel_main() {
    arch::serial::init();
    log_add_sink(arch::serial::putc);
    kprintf("%s", BANNER);

    if (!g_hhdm.response)   panic("bootloader did not provide HHDM");
    if (!g_memmap.response) panic("bootloader did not provide a memory map");

    if (g_kaddr.response) {
        kprintf("[boot] kernel: phys %p, virt %p\n",
                (void*)g_kaddr.response->physical_base,
                (void*)g_kaddr.response->virtual_base);
    }
    kprintf("[boot] HHDM offset = %p\n", (void*)g_hhdm.response->offset);
    kprintf("[boot] ACPI RSDP = %p\n", g_rsdp.response ? g_rsdp.response->address : nullptr);

    arch::gdt::init();
    arch::idt::init();
    arch::pic::init();
    arch::idt::set_handler(3, on_breakpoint);

    mm::pmm::init(g_memmap.response, g_hhdm.response->offset);
    mm::heap::init();
    nvmfs::init();

    if (g_fb.response && g_fb.response->framebuffer_count > 0) {
        if (video::init(g_fb.response->framebuffers[0])) {
            video::console_init();
            log_add_sink(video::console_putc);
            kprintf("C.O.N.D.R. framebuffer console\n");
        }
    } else {
        kprintf("[fb] framebuffer unavailable\n");
    }

    arch::timer::init();
    arch::keyboard::init();
    arch::pic::unmask(0);
    arch::pic::unmask(1);
    arch::sti();

    test_subsystems();

    arch::scheduler::init();
    u32 shell_id;
    u32 worker_id;
    if (!arch::scheduler::create("shell", shell_task, shell_id)) panic("scheduler: cannot create shell task");
    if (!arch::scheduler::create("worker", worker_task, worker_id)) panic("scheduler: cannot create worker task");
    kprintf("\n[boot] initialization complete. Starting scheduler.\n");
    arch::scheduler::run();
}