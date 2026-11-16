#pragma once
#include <nord/types.hpp>

namespace nord {

using SinkFn = void (*)(char);
void log_add_sink(SinkFn fn);

void kprintf(const char* fmt, ...);

void panic_set_frame(const void* frame);

[[noreturn]] void panic(const char* fmt, ...);

}