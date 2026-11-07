#pragma once
#include <arch/x86_64/cpu/idt.hpp>
#include <mm/address_space.hpp>
#include <nord/types.hpp>

namespace nord::arch::scheduler {

enum class State : u8 {
    ready,
    running,
    blocked,
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
bool create_user(const char* name, mm::address_space::AddressSpace& space,
                 u64 entry, u64 user_stack, u32& id);
[[noreturn]] void run();
void yield();
void block_current();
void wake(u32 id);
void mark_current_exited();
[[noreturn]] void exit_current();
void preempt_disable();
void preempt_enable();
Registers* schedule(Registers* frame);
usize count();
u32 current_id();
bool at(usize position, TaskInfo& info);
mm::address_space::AddressSpace* current_address_space();

}