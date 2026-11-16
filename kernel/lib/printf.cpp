#include <lib/print.hpp>
#include <lib/string.hpp>
#include <lib/symbols.hpp>
#include <arch/x86_64/cpu/idt.hpp>
#include <arch/x86_64/io.hpp>
#include <mm/pmm.hpp>
#include <stdarg.h>

extern "C" const nord::u8 __kernel_start[];
extern "C" const nord::u8 __kernel_end[];

namespace nord {

static constexpr usize MAX_SINKS = 4;
static SinkFn g_sinks[MAX_SINKS];
static usize  g_sink_count = 0;
static const arch::Registers* g_panic_frame;

void panic_set_frame(const void* frame) {
    g_panic_frame = static_cast<const arch::Registers*>(frame);
}

void log_add_sink(SinkFn fn) {
    if (g_sink_count < MAX_SINKS) g_sinks[g_sink_count++] = fn;
}

static void emit(char c) {
    for (usize i = 0; i < g_sink_count; ++i) g_sinks[i](c);
}

static void emit_str(const char* s) { while (*s) emit(*s++); }

static void emit_uint(u64 v, u32 base, bool upper, usize pad, char pad_ch) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[64];
    if (pad > sizeof(buf)) pad = sizeof(buf);
    usize n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = digits[v % base]; v /= base; }
    while (n < pad) buf[n++] = pad_ch;
    while (n--) emit(buf[n]);
}

static void emit_int(i64 v, usize pad, char pad_ch) {
    if (v < 0) { emit('-'); v = -v; }
    emit_uint(static_cast<u64>(v), 10, false, pad, pad_ch);
}

static void vkprintf(const char* fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { emit(*fmt++); continue; }
        ++fmt;

        char pad_ch = ' ';
        usize pad = 0;
        bool alt = false;
        if (*fmt == '#') { alt = true; ++fmt; }
        if (*fmt == '0') { pad_ch = '0'; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') { pad = pad * 10 + static_cast<usize>(*fmt - '0'); ++fmt; }

        int longness = 0;
        while (*fmt == 'l') { ++longness; ++fmt; }
        if (*fmt == 'z') { longness = 2; ++fmt; }

        switch (*fmt) {
        case 's': emit_str(va_arg(ap, const char*)); break;
        case 'c': emit(static_cast<char>(va_arg(ap, int))); break;
        case 'd': case 'i':
            emit_int(longness ? va_arg(ap, i64) : va_arg(ap, i32), pad, pad_ch);
            break;
        case 'u':
            emit_uint(longness ? va_arg(ap, u64) : va_arg(ap, u32), 10, false, pad, pad_ch);
            break;
        case 'x':
            if (alt) emit_str("0x");
            emit_uint(longness ? va_arg(ap, u64) : va_arg(ap, u32), 16, false, pad, pad_ch);
            break;
        case 'X':
            if (alt) emit_str("0X");
            emit_uint(longness ? va_arg(ap, u64) : va_arg(ap, u32), 16, true, pad, pad_ch);
            break;
        case 'p':
            emit_str("0x");
            emit_uint(reinterpret_cast<u64>(va_arg(ap, void*)), 16, false, 16, '0');
            break;
        case 'b':
            if (alt) emit_str("0b");
            emit_uint(longness ? va_arg(ap, u64) : va_arg(ap, u32), 2, false, pad, pad_ch);
            break;
        case '%': emit('%'); break;
        default:  emit('%'); emit(*fmt); break;
        }
        ++fmt;
    }
}

void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
}

static void emit_symbol(u64 address) {
    const char* name = nullptr;
    u64 offset = 0;
    if (resolve_symbol(address, &name, &offset)) {
        emit_str(name);
        if (offset) {
            emit_str("+0x");
            emit_uint(offset, 16, false, 0, ' ');
        }
    } else {
        emit_str("<unknown>");
    }
}

static void emit_register(const char* name, u64 value) {
    emit_str("  ");
    emit_str(name);
    emit_str(" = 0x");
    emit_uint(value, 16, false, 16, '0');
}

static void dump_registers(const arch::Registers* frame, u64 rsp, u64 rbp) {
    if (!frame) {
        emit_register("RSP", rsp);
        emit('\n');
        emit_register("RBP", rbp);
        emit('\n');
        return;
    }

    emit_register("RAX", frame->rax); emit_register("RBX", frame->rbx); emit('\n');
    emit_register("RCX", frame->rcx); emit_register("RDX", frame->rdx); emit('\n');
    emit_register("RSI", frame->rsi); emit_register("RDI", frame->rdi); emit('\n');
    emit_register("RBP", frame->rbp); emit_register("RSP", frame->rsp); emit('\n');
    emit_register("R8 ", frame->r8);  emit_register("R9 ", frame->r9);  emit('\n');
    emit_register("R10", frame->r10); emit_register("R11", frame->r11); emit('\n');
    emit_register("R12", frame->r12); emit_register("R13", frame->r13); emit('\n');
    emit_register("R14", frame->r14); emit_register("R15", frame->r15); emit('\n');
    emit_register("RIP", frame->rip); emit_register("RFL", frame->rflags); emit('\n');
    emit_register("CS ", frame->cs);  emit_register("SS ", frame->ss);  emit('\n');
    emit_register("CR2", arch::read_cr2()); emit_register("ERR", frame->error_code); emit('\n');
}

static void dump_page_fault(const arch::Registers* frame) {
    if (!frame || frame->vector != 14) return;
    const u64 error = frame->error_code;
    emit_str("Page fault: ");
    emit_str((error & 1) ? "protection violation" : "page not present");
    emit_str((error & 2) ? ", write" : ", read");
    emit_str((error & 4) ? ", ring 3" : ", ring 0");
    if (error & 16) emit_str(", instruction fetch");
    emit('\n');
}

static constexpr u64 PAGE_PRESENT_BIT = 1;
static constexpr u64 PAGE_LARGE_BIT = 0x80;
static constexpr u64 PAGE_ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;

static bool mapped_in_current_space(u64 address) {
    const u64 hhdm = mm::pmm::hhdm();
    const u64 root = arch::read_cr3() & PAGE_ADDRESS_MASK;
    if (!hhdm || !root) return false;

    auto table = [hhdm](u64 physical) {
        return reinterpret_cast<const u64*>(physical + hhdm);
    };

    const u64 e4 = table(root)[(address >> 39) & 0x1FF];
    if (!(e4 & PAGE_PRESENT_BIT) || (e4 & PAGE_LARGE_BIT)) return false;
    const u64 e3 = table(e4 & PAGE_ADDRESS_MASK)[(address >> 30) & 0x1FF];
    if (!(e3 & PAGE_PRESENT_BIT)) return false;
    if (e3 & PAGE_LARGE_BIT) return true;
    const u64 e2 = table(e3 & PAGE_ADDRESS_MASK)[(address >> 21) & 0x1FF];
    if (!(e2 & PAGE_PRESENT_BIT)) return false;
    if (e2 & PAGE_LARGE_BIT) return true;
    const u64 e1 = table(e2 & PAGE_ADDRESS_MASK)[(address >> 12) & 0x1FF];
    return (e1 & PAGE_PRESENT_BIT) != 0;
}

static bool in_kernel_image(u64 address) {
    return address >= reinterpret_cast<u64>(__kernel_start)
        && address < reinterpret_cast<u64>(__kernel_end);
}

static bool valid_frame(u64 rbp, u64 rsp, u64 previous) {
    if (!rbp || (rbp & 0x7)) return false;
    if ((rbp >> 48) != 0xFFFF) return false;
    if (rsp && rbp < rsp) return false;
    if (rsp && rbp - rsp > 1024 * 1024) return false;
    if (previous && rbp <= previous) return false;
    return mapped_in_current_space(rbp) && mapped_in_current_space(rbp + 8);
}

static void emit_stack_frame(usize index, u64 address) {
    emit_str("  #");
    emit_uint(index, 10, false, 2, '0');
    emit_str(" 0x");
    emit_uint(address, 16, false, 16, '0');
    emit_str("  ");
    emit_symbol(address);
    emit('\n');
}

static void dump_stack_trace(const arch::Registers* frame, u64 rsp, u64 rbp) {
    usize index = 0;
    u64 previous = 0;

    if (frame) {
        emit_stack_frame(index++, frame->rip);
        rsp = frame->rsp;
        rbp = frame->rbp;
    }

    for (usize depth = 0; depth < 16 && valid_frame(rbp, rsp, previous); ++depth) {
        const auto* words = reinterpret_cast<const u64*>(rbp);
        const u64 next_rbp = words[0];
        const u64 return_address = words[1];
        if (!in_kernel_image(return_address)) break;
        emit_stack_frame(index++, return_address);
        previous = rbp;
        rbp = next_rbp;
    }

    if (index == 0) emit_str("  <no frames>\n");
}

[[noreturn]] void panic(const char* fmt, ...) {
    arch::cli();
    emit_str("\n*** C.O.N.D.R. PANIC ***\n");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    emit('\n');

    u64 rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    const u64 rbp = reinterpret_cast<u64>(__builtin_frame_address(0));
    const arch::Registers* frame = g_panic_frame;

    emit_str("\nRegisters:\n");
    dump_registers(frame, rsp, rbp);
    dump_page_fault(frame);
    emit_str("Call stack:\n");
    dump_stack_trace(frame, rsp, rbp);
    arch::halt_forever();
}

}