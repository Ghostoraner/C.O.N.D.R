#pragma once
#include <nord/types.hpp>

namespace nord::arch::timer {

void init(u32 frequency_hz = 100);
u64 ticks();

}