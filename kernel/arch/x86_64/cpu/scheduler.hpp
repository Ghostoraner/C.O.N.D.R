#pragma once
#include <nord/types.hpp>

namespace nord::arch::scheduler {

enum class State : u8 {
    ready,
    running,
    exited,
};

struct TaskInfo {
    u32 id;
    const char* name;
    State state;
    u64 switches;
};

using Entry = void (*)();

void init();
bool create(const char* name, Entry entry, u32& id);
[[noreturn]] void run();
void yield();
usize count();
bool at(usize position, TaskInfo& info);

}