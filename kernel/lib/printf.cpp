#include <lib/print.hpp>
#include <lib/string.hpp>
#include <arch/x86_64/io.hpp>
#include <stdarg.h>

namespace nord {

static constexpr usize MAX_SINKS = 4;
static SinkFn g_sinks[MAX_SINKS];
static usize  g_sink_count = 0;

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

[[noreturn]] void panic(const char* fmt, ...) {
    arch::cli();
    emit_str("\n*** C.O.N.D.R. PANIC ***\n");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    emit('\n');
    arch::halt_forever();
}

}