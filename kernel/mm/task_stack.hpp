#pragma once
#include <nord/types.hpp>

namespace nord::mm::task_stack {

bool init(usize stack_size);
void* allocate();
u64 top(const void* base);
void release(void* base);

}
