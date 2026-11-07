#pragma once
#include <arch/x86_64/cpu/idt.hpp>

namespace nord::arch::syscall {

void dispatch(Registers* registers);

}