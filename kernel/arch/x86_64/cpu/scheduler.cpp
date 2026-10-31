#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace nord::arch::scheduler {

static constexpr usize MAX_TASKS = 8;
static constexpr usize STACK_SIZE = 16 * 1024;
static constexpr usize NAME_SIZE = 24;

struct Task {
    u32 id;
    bool active;
    State state;
    Entry entry;
    u64* stack_pointer;
    void* stack;
    u64 switches;
    char name[NAME_SIZE];
};

static Task g_tasks[MAX_TASKS];
static usize g_count;
static u32 g_next_id;
static Task* g_current;
static u64* g_boot_stack_pointer;
static bool g_running;

extern "C" void arch_context_switch(u64** old_stack, u64** new_stack);

static Task* next_ready() {
    const usize start = g_current
        ? static_cast<usize>(g_current - g_tasks + 1) % MAX_TASKS : 0;
    for (usize offset = 0; offset < MAX_TASKS; ++offset) {
        Task& task = g_tasks[(start + offset) % MAX_TASKS];
        if (task.active && task.state == State::ready) return &task;
    }
    return nullptr;
}

[[noreturn]] static void task_bootstrap() {
    Task* task = g_current;
    task->entry();
    task->state = State::exited;
    for (;;) yield();
}

void init() {
    memset(g_tasks, 0, sizeof(g_tasks));
    g_count = 0;
    g_next_id = 1;
    g_current = nullptr;
    g_boot_stack_pointer = nullptr;
    g_running = false;
}

bool create(const char* name, Entry entry, u32& id) {
    if (!name || !*name || !entry || g_count == MAX_TASKS) return false;
    Task& task = g_tasks[g_count];
    void* stack = mm::heap::kmalloc(STACK_SIZE);
    if (!stack) return false;

    memset(&task, 0, sizeof(task));
    task.id = g_next_id++;
    task.active = true;
    task.state = State::ready;
    task.entry = entry;
    task.stack = stack;
    task.switches = 0;
    usize name_length = strlen(name);
    if (name_length >= NAME_SIZE) name_length = NAME_SIZE - 1;
    memcpy(task.name, name, name_length);
    task.name[name_length] = '\0';

    u64 stack_top = reinterpret_cast<u64>(stack) + STACK_SIZE;
    stack_top = (stack_top & ~static_cast<u64>(0xF)) - sizeof(u64);
    task.stack_pointer = reinterpret_cast<u64*>(stack_top) - 7;
    memset(task.stack_pointer, 0, 6 * sizeof(u64));
    task.stack_pointer[6] = reinterpret_cast<u64>(task_bootstrap);

    ++g_count;
    id = task.id;
    return true;
}

void yield() {
    if (!g_running) return;
    Task* previous = g_current;
    if (previous && previous->state == State::running)
        previous->state = State::ready;

    Task* next = next_ready();
    if (!next) {
        if (previous) {
            g_current = nullptr;
            arch_context_switch(&previous->stack_pointer, &g_boot_stack_pointer);
        } else {
            hlt();
        }
        return;
    }

    if (next == previous) {
        next->state = State::running;
        return;
    }

    next->state = State::running;
    ++next->switches;
    g_current = next;
    u64** old_stack = previous ? &previous->stack_pointer : &g_boot_stack_pointer;
    arch_context_switch(old_stack, &next->stack_pointer);
}

[[noreturn]] void run() {
    g_running = true;
    for (;;) yield();
}

usize count() {
    return g_count;
}

bool at(usize position, TaskInfo& info) {
    if (position >= g_count || !g_tasks[position].active) return false;
    const Task& task = g_tasks[position];
    info = {task.id, task.name, task.state, task.switches};
    return true;
}

}