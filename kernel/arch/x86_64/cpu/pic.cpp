#include <arch/x86_64/cpu/pic.hpp>
#include <arch/x86_64/io.hpp>

namespace nord::arch::pic {

static constexpr u16 MASTER_COMMAND = 0x20;
static constexpr u16 MASTER_DATA = 0x21;
static constexpr u16 SLAVE_COMMAND = 0xA0;
static constexpr u16 SLAVE_DATA = 0xA1;
static constexpr u8 MASTER_OFFSET = 0x20;
static constexpr u8 SLAVE_OFFSET = 0x28;
static u8 g_master_mask;
static u8 g_slave_mask;

void init() {
    g_master_mask = 0xFF;
    g_slave_mask = 0xFF;

    outb(MASTER_COMMAND, 0x11);
    io_wait();
    outb(SLAVE_COMMAND, 0x11);
    io_wait();
    outb(MASTER_DATA, MASTER_OFFSET);
    io_wait();
    outb(SLAVE_DATA, SLAVE_OFFSET);
    io_wait();
    outb(MASTER_DATA, 0x04);
    io_wait();
    outb(SLAVE_DATA, 0x02);
    io_wait();
    outb(MASTER_DATA, 0x01);
    io_wait();
    outb(SLAVE_DATA, 0x01);
    io_wait();
    outb(MASTER_DATA, g_master_mask);
    outb(SLAVE_DATA, g_slave_mask);
}

void unmask(u8 irq) {
    if (irq < 8) {
        g_master_mask &= static_cast<u8>(~(1u << irq));
        outb(MASTER_DATA, g_master_mask);
    } else if (irq < 16) {
        g_slave_mask &= static_cast<u8>(~(1u << (irq - 8)));
        g_master_mask &= static_cast<u8>(~(1u << 2));
        outb(SLAVE_DATA, g_slave_mask);
        outb(MASTER_DATA, g_master_mask);
    }
}

void send_eoi(u8 irq) {
    if (irq >= 8) outb(SLAVE_COMMAND, 0x20);
    outb(MASTER_COMMAND, 0x20);
}

}