#pragma once
#include <nord/types.hpp>

namespace nord {

struct KernelSymbol {
    u64 address;
    u64 size;
    const char* name;
};

extern "C" const KernelSymbol __kernel_symbols[] __attribute__((weak));
extern "C" const u64 __kernel_symbol_count __attribute__((weak));

bool resolve_symbol(u64 address, const char** name, u64* offset);

}
