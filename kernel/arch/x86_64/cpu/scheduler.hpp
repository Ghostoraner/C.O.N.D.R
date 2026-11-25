#pragma once
#include <arch/x86_64/cpu/idt.hpp>
#include <mm/address_space.hpp>
#include <nord/types.hpp>

namespace nord::arch::scheduler {

enum class State : u8 {
    ready,
    running,
    blocked,
    zombie,
};

struct TaskInfo {
    u32 id;
    u32 parent;
    i32 status;
    const char* name;
    State state;
    u64 switches;
};

using Entry = void (*)();

// The task table keeps a dynamically grown index of task pointers while the
// task records themselves come from a configurable slab cache. `configure`
// must be called before `init` to take effect.
struct Config {
    usize initial_capacity = 8;
    usize tasks_per_slab = 8;
    usize stack_size = 16 * 1024;
};

void configure(const Config& config);
void init();
bool create(const char* name, Entry entry, u32& id);
bool create_user(const char* name, mm::address_space::AddressSpace& space,
                 u64 entry, u64 user_stack, u32& id);
[[noreturn]] void run();
void yield();
void block_current();
void wake(u32 id);
void sleep_ms(u64 milliseconds);
void mark_current_exited(i32 status);
[[noreturn]] void exit_current(i32 status);
bool wait(i32 pid, i32& status, u32& child_id);
void preempt_disable();
void preempt_enable();
Registers* schedule(Registers* frame);
usize count();
usize capacity();
usize slab_capacity();
u32 current_id();
bool at(usize position, TaskInfo& info);
mm::address_space::AddressSpace* current_address_space();

}