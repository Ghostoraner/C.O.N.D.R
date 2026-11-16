#include <lib/symbols.hpp>

namespace nord {

extern "C" const u8 __kernel_start[];
extern "C" const u8 __kernel_end[];

bool resolve_symbol(u64 address, const char** name, u64* offset) {
    if (!__kernel_symbol_count || !__kernel_symbols) return false;
    const u64 start = reinterpret_cast<u64>(__kernel_start);
    const u64 end = reinterpret_cast<u64>(__kernel_end);
    if (address < start || address >= end) return false;

    usize low = 0;
    usize high = static_cast<usize>(__kernel_symbol_count);
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (__kernel_symbols[middle].address <= address) low = middle + 1;
        else high = middle;
    }
    if (low == 0) return false;

    const KernelSymbol& symbol = __kernel_symbols[low - 1];
    if (name) *name = symbol.name;
    if (offset) *offset = address - symbol.address;
    return true;
}

}
