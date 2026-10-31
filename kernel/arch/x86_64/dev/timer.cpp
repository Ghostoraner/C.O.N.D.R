#include <arch/x86_64/dev/timer.hpp>
#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/io.hpp>

namespace nord::arch::timer {

static constexpr u16 PIT_CHANNEL_0 = 0x40;
static constexpr u16 PIT_COMMAND = 0x43;
static constexpr u32 PIT_INPUT_HZ = 1193182;
static u64 g_ticks;

static void on_tick(Registers*) {
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
}

void init(u32 frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 100;
    u32 divisor = PIT_INPUT_HZ / frequency_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    idt::set_handler(0x20, on_tick);
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL_0, static_cast<u8>(divisor & 0xFF));
    outb(PIT_CHANNEL_0, static_cast<u8>((divisor >> 8) & 0xFF));
}

u64 ticks() {
    return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
}

}