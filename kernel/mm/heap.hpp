#pragma once
#include <nord/types.hpp>

namespace nord::mm::heap {

void  init();
void* kmalloc(usize size);
void* kcalloc(usize count, usize size);
void* krealloc(void* ptr, usize size);
void  kfree(void* ptr);

void  stats(u64& used, u64& capacity);

}