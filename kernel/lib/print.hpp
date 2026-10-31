#pragma once
#include <nord/types.hpp>

namespace nord {

using SinkFn = void (*)(char);
void log_add_sink(SinkFn fn);

void kprintf(const char* fmt, ...);

[[noreturn]] void panic(const char* fmt, ...);

}