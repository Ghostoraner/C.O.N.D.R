#include <arch/x86_64/dev/serial.hpp>
#include <arch/x86_64/io.hpp>

namespace nord::arch::serial {

static constexpr u16 COM1 = 0x3F8;

enum Reg : u16 {
    DATA = 0, IER = 1, FCR = 2, LCR = 3, MCR = 4, LSR = 5,
    DLL = 0, DLM = 1,
};

static bool g_ready = false;

bool init() {
    outb(COM1 + IER, 0x00);
    outb(COM1 + LCR, 0x80);
    outb(COM1 + DLL, 0x03);
    outb(COM1 + DLM, 0x00);
    outb(COM1 + LCR, 0x03);
    outb(COM1 + FCR, 0xC7);
    outb(COM1 + MCR, 0x0B);

    outb(COM1 + MCR, 0x1E);
    outb(COM1 + DATA, 0xAE);
    if (inb(COM1 + DATA) != 0xAE) { g_ready = false; return false; }

    outb(COM1 + MCR, 0x0F);
    g_ready = true;
    return true;
}

static bool tx_empty() { return (inb(COM1 + LSR) & 0x20) != 0; }

void putc(char c) {
    if (!g_ready) return;
    if (c == '\n') putc('\r');
    while (!tx_empty()) { }
    outb(COM1 + DATA, static_cast<u8>(c));
}

void write(const char* s) { while (*s) putc(*s++); }

bool try_getc(char& c) {
    if (!g_ready || (inb(COM1 + LSR) & 0x01) == 0) return false;
    c = static_cast<char>(inb(COM1 + DATA));
    return true;
}

char getc() {
    char c;
    while (!try_getc(c)) { }
    return c;
}

}