#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/cpu/gdt.hpp>
#include <arch/x86_64/io.hpp>
#include <ipc/ipc.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
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
    Registers* frame;
    void* stack;
    bool user_mode;
    mm::address_space::AddressSpace space;
    u64 switches;
    char name[NAME_SIZE];
};

static Task g_tasks[MAX_TASKS];
static usize g_count;
static u32 g_next_id;
static Task* g_current;
static Registers* g_boot_frame;
static Task* g_retired_task;
static bool g_running;
static u32 g_preempt_depth;
static paddr_t g_kernel_cr3;
static u64 g_boot_rsp0;

static constexpr u64 INITIAL_FRAME_WORDS = 22;

extern "C" void isr_yield();

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
    exit_current();
}

static Task* allocate_task(const char* name, u32& id) {
    if (!name || !*name || g_count == MAX_TASKS) return nullptr;
    usize slot = 0;
    while (slot < MAX_TASKS && g_tasks[slot].active) ++slot;
    if (slot == MAX_TASKS) return nullptr;
    void* stack = mm::heap::kmalloc(STACK_SIZE);
    if (!stack) return nullptr;

    Task& task = g_tasks[slot];
    memset(&task, 0, sizeof(task));
    task.id = g_next_id++;
    task.active = true;
    task.state = State::ready;
    task.stack = stack;
    const usize length = strlen(name) < NAME_SIZE - 1 ? strlen(name) : NAME_SIZE - 1;
    memcpy(task.name, name, length);
    task.name[length] = '\0';
    ++g_count;
    id = task.id;
    return &task;
}

static Registers* initial_frame(Task& task, u64 entry, u64 code_selector,
                                u64 stack_pointer, u64 data_selector) {
    u64 kernel_stack_top = reinterpret_cast<u64>(task.stack) + STACK_SIZE;
    kernel_stack_top = (kernel_stack_top & ~static_cast<u64>(0xF)) - sizeof(u64);
    task.frame = reinterpret_cast<Registers*>(kernel_stack_top - INITIAL_FRAME_WORDS * sizeof(u64));
    memset(task.frame, 0, INITIAL_FRAME_WORDS * sizeof(u64));
    u64* words = reinterpret_cast<u64*>(task.frame);
    words[15] = 48;
    words[17] = entry;
    words[18] = code_selector;
    words[19] = 0x202;
    words[20] = stack_pointer;
    words[21] = data_selector;
    return task.frame;
}

static void reap_retired_task() {
    if (!g_retired_task) return;
    Task* task = g_retired_task;
    ipc::close_task(task->id);
    if (task->user_mode) mm::address_space::destroy(task->space);
    if (task->stack) mm::heap::kfree(task->stack);
    memset(task, 0, sizeof(*task));
    --g_count;
    g_retired_task = nullptr;
}

void init() {
    memset(g_tasks, 0, sizeof(g_tasks));
    g_count = 0;
    g_next_id = 1;
    g_current = nullptr;
    g_boot_frame = nullptr;
    g_retired_task = nullptr;
    g_running = false;
    g_preempt_depth = 0;
    g_kernel_cr3 = arch::read_cr3() & 0x000FFFFFFFFFF000ULL;
    asm volatile("mov %%rsp, %0" : "=r"(g_boot_rsp0));
}

bool create(const char* name, Entry entry, u32& id) {
    if (!entry) return false;
    Task* task = allocate_task(name, id);
    if (!task) return false;
    task->entry = entry;
    u64 stack_top = reinterpret_cast<u64>(task->stack) + STACK_SIZE;
    stack_top = (stack_top & ~static_cast<u64>(0xF)) - sizeof(u64);
    initial_frame(*task, reinterpret_cast<u64>(task_bootstrap), gdt::KERNEL_CODE,
                  stack_top, gdt::KERNEL_DATA);
    return true;
}

bool create_user(const char* name, mm::address_space::AddressSpace& space,
                 u64 entry, u64 user_stack, u32& id) {
    if (!space.root || !mm::address_space::contains(space, entry, 1)
        || !mm::address_space::contains(space, user_stack - 1, 1)) return false;
    Task* task = allocate_task(name, id);
    if (!task) return false;
    task->user_mode = true;
    task->space = space;
    space.root = 0;
    initial_frame(*task, entry, gdt::USER_CODE, user_stack, gdt::USER_DATA);
    return true;
}

void preempt_disable() {
    ++g_preempt_depth;
}

void preempt_enable() {
    if (g_preempt_depth) --g_preempt_depth;
}

Registers* schedule(Registers* frame) {
    if (!g_running || g_preempt_depth) return frame;

    reap_retired_task();

    Task* previous = g_current;
    if (previous) {
        if (previous->state == State::running) previous->state = State::ready;
        if (previous->state != State::exited) previous->frame = frame;
    } else {
        g_boot_frame = frame;
    }

    Task* next = next_ready();
    if (!next) {
        g_current = nullptr;
        if (previous && previous->state == State::exited) g_retired_task = previous;
        if ((arch::read_cr3() & 0x000FFFFFFFFFF000ULL) != g_kernel_cr3)
            arch::write_cr3(g_kernel_cr3);
        gdt::set_kernel_stack(g_boot_rsp0);
        return g_boot_frame ? g_boot_frame : frame;
    }

    if (previous && previous->state == State::exited) g_retired_task = previous;

    next->state = State::running;
    ++next->switches;
    g_current = next;
    const paddr_t next_cr3 = next->user_mode ? next->space.root : g_kernel_cr3;
    if ((arch::read_cr3() & 0x000FFFFFFFFFF000ULL) != next_cr3)
        arch::write_cr3(next_cr3);
    const u64 kernel_stack_top = reinterpret_cast<u64>(next->stack) + STACK_SIZE;
    gdt::set_kernel_stack(kernel_stack_top);
    return next->frame;
}

void yield() {
    if (g_running) isr_yield();
}

void block_current() {
    if (!g_running || !g_current) return;
    cli();
    g_current->state = State::blocked;
    isr_yield();
    sti();
}

void wake(u32 id) {
    for (usize i = 0; i < MAX_TASKS; ++i) {
        Task& task = g_tasks[i];
        if (task.active && task.id == id && task.state == State::blocked) {
            task.state = State::ready;
            return;
        }
    }
}

void mark_current_exited() {
    if (g_current) g_current->state = State::exited;
}

[[noreturn]] void exit_current() {
    if (g_current) g_current->state = State::exited;
    isr_yield();
    halt_forever();
}

[[noreturn]] void run() {
    g_running = true;
    yield();
    for (;;) hlt();
}

usize count() {
    return g_count;
}

u32 current_id() {
    return g_current ? g_current->id : 0;
}

bool at(usize position, TaskInfo& info) {
    for (usize i = 0; i < MAX_TASKS; ++i) {
        const Task& task = g_tasks[i];
        if (!task.active) continue;
        if (position-- != 0) continue;
        info = {task.id, task.name, task.state, task.switches};
        return true;
    }
    return false;
}

mm::address_space::AddressSpace* current_address_space() {
    return g_current && g_current->user_mode ? &g_current->space : nullptr;
}

}