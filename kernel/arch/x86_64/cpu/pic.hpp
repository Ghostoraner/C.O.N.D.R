#pragma once
#include <nord/types.hpp>

namespace nord::arch::pic {

void init();
void unmask(u8 irq);
void send_eoi(u8 irq);

}