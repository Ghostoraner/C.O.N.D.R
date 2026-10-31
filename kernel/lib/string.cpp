#include <lib/string.hpp>

using namespace nord;

extern "C" void* memcpy(void* dst, const void* src, usize n) {
    auto* d = static_cast<u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    for (usize i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

extern "C" void* memset(void* dst, int c, usize n) {
    auto* d = static_cast<u8*>(dst);
    for (usize i = 0; i < n; ++i) d[i] = static_cast<u8>(c);
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, usize n) {
    auto* d = static_cast<u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    if (d == s || n == 0) return dst;
    if (d < s) { for (usize i = 0; i < n; ++i) d[i] = s[i]; }
    else       { for (usize i = n; i-- > 0;)  d[i] = s[i]; }
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, usize n) {
    auto* x = static_cast<const u8*>(a);
    auto* y = static_cast<const u8*>(b);
    for (usize i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

extern "C" usize strlen(const char* s) { usize n = 0; while (s[n]) ++n; return n; }

extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<u8>(*a) - static_cast<u8>(*b);
}

extern "C" int strncmp(const char* a, const char* b, usize n) {
    for (usize i = 0; i < n; ++i) {
        if (a[i] != b[i]) return static_cast<u8>(a[i]) - static_cast<u8>(b[i]);
        if (!a[i]) break;
    }
    return 0;
}