#pragma once
#include <nord/types.hpp>

namespace nord::arch::gdt {

constexpr u16 KERNEL_CODE = 0x08;
constexpr u16 KERNEL_DATA = 0x10;
constexpr u16 USER_DATA   = 0x18 | 3;
constexpr u16 USER_CODE   = 0x20 | 3;
constexpr u16 TSS_SELECTOR = 0x28;

void init();
void set_kernel_stack(u64 stack_top);

}