#pragma once
#include <nord/types.hpp>

namespace nord::arch::fpu {

constexpr usize STATE_SIZE = 512;
constexpr usize STATE_ALIGN = 16;

void init();
void reset(void* state);
void save(void* state);
void restore(const void* state);

}
