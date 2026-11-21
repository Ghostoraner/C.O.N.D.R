#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/cpu/fpu.hpp>
#include <arch/x86_64/cpu/gdt.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/io.hpp>
#include <ipc/ipc.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <mm/heap.hpp>
#include <mm/task_stack.hpp>

namespace nord::arch::scheduler {

static constexpr usize STACK_SIZE = 16 * 1024;
static constexpr usize NAME_SIZE = 24;

struct Task {
    u32 id;
    bool active;
    State state;
    Entry entry;
    Registers* frame;
    alignas(fpu::STATE_ALIGN) u8 fpu_state[fpu::STATE_SIZE];
    void* stack;
    bool user_mode;
    mm::address_space::AddressSpace space;
    u64 switches;
    char name[NAME_SIZE];
};

static Task** g_tasks;
static usize g_task_capacity;
static usize g_count;
static u32 g_next_id;
static Task* g_current;
static Registers* g_boot_frame;
alignas(fpu::STATE_ALIGN) static u8 g_boot_fpu[fpu::STATE_SIZE];
static Task* g_retired_task;
static bool g_running;
static u32 g_preempt_depth;
static paddr_t g_kernel_cr3;
static u64 g_boot_rsp0;

static constexpr u64 INITIAL_FRAME_WORDS = 22;

extern "C" void isr_yield();

// IRQ handlers call wake(), which walks g_tasks. Table growth must therefore
// keep interrupts disabled until both the array pointer and capacity are
// published, otherwise an IRQ can observe a freed array.
struct InterruptGuard {
    InterruptGuard() {
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    }
    ~InterruptGuard() {
        asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
    }
    u64 flags;
};

static bool grow_tasks(usize needed) {
    if (needed <= g_task_capacity) return true;
    usize capacity = g_task_capacity ? g_task_capacity : 8;
    while (capacity < needed) {
        if (capacity > ~static_cast<usize>(0) / 2) return false;
        capacity *= 2;
    }
    InterruptGuard guard;
    auto** tasks = static_cast<Task**>(
        mm::heap::krealloc(g_tasks, capacity * sizeof(Task*)));
    if (!tasks) return false;
    for (usize i = g_task_capacity; i < capacity; ++i) tasks[i] = nullptr;
    g_tasks = tasks;
    g_task_capacity = capacity;
    return true;
}

static bool reserve_task_slot(usize& slot) {
    for (usize i = 0; i < g_task_capacity; ++i) {
        if (!g_tasks[i]) { slot = i; return true; }
    }
    const usize previous = g_task_capacity;
    if (!grow_tasks(g_task_capacity ? g_task_capacity + 1 : 1)) return false;
    slot = previous;
    return true;
}

static void free_task_table() {
    if (!g_tasks) return;
    for (usize i = 0; i < g_task_capacity; ++i) mm::heap::kfree(g_tasks[i]);
    mm::heap::kfree(g_tasks);
    g_tasks = nullptr;
    g_task_capacity = 0;
}

static Task* next_ready() {
    if (!g_task_capacity) return nullptr;
    usize start = 0;
    if (g_current) {
        for (usize i = 0; i < g_task_capacity; ++i) {
            if (g_tasks[i] == g_current) {
                start = (i + 1) % g_task_capacity;
                break;
            }
        }
    }
    for (usize offset = 0; offset < g_task_capacity; ++offset) {
        Task* task = g_tasks[(start + offset) % g_task_capacity];
        if (task && task->active && task->state == State::ready) return task;
    }
    return nullptr;
}

[[noreturn]] static void task_bootstrap() {
    Task* task = g_current;
    task->entry();
    exit_current();
}

static Task* allocate_task(const char* name, u32& id) {
    if (!name || !*name) return nullptr;
    usize slot = 0;
    if (!reserve_task_slot(slot)) return nullptr;

    void* stack = mm::task_stack::allocate();
    if (!stack) return nullptr;
    auto* task = static_cast<Task*>(mm::heap::kmalloc(sizeof(Task)));
    if (!task) {
        mm::task_stack::release(stack);
        return nullptr;
    }

    memset(task, 0, sizeof(*task));
    task->id = g_next_id++;
    task->active = true;
    task->state = State::ready;
    task->stack = stack;
    fpu::reset(task->fpu_state);
    const usize length = strlen(name) < NAME_SIZE - 1 ? strlen(name) : NAME_SIZE - 1;
    memcpy(task->name, name, length);
    task->name[length] = '\0';
    g_tasks[slot] = task;
    ++g_count;
    id = task->id;
    return task;
}

static Registers* initial_frame(Task& task, u64 entry, u64 code_selector,
                                u64 stack_pointer, u64 data_selector) {
    u64 kernel_stack_top = mm::task_stack::top(task.stack);
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
    // A task that exited while blocked in a keyboard read must not leave a
    // stale waiter registration behind.
    keyboard::clear_waiter(task->id);
    if (task->user_mode) mm::address_space::destroy(task->space);
    if (task->stack) mm::task_stack::release(task->stack);
    for (usize i = 0; i < g_task_capacity; ++i) {
        if (g_tasks[i] == task) {
            g_tasks[i] = nullptr;
            break;
        }
    }
    mm::heap::kfree(task);
    --g_count;
    g_retired_task = nullptr;
}

void init() {
    free_task_table();
    g_count = 0;
    g_next_id = 1;
    g_current = nullptr;
    g_boot_frame = nullptr;
    fpu::reset(g_boot_fpu);
    g_retired_task = nullptr;
    g_running = false;
    g_preempt_depth = 0;
    g_kernel_cr3 = arch::read_cr3() & 0x000FFFFFFFFFF000ULL;
    asm volatile("mov %%rsp, %0" : "=r"(g_boot_rsp0));
    mm::task_stack::init(STACK_SIZE);
}

bool create(const char* name, Entry entry, u32& id) {
    if (!entry) return false;
    preempt_disable();
    Task* task = allocate_task(name, id);
    if (!task) {
        preempt_enable();
        return false;
    }
    task->entry = entry;
    u64 stack_top = mm::task_stack::top(task->stack);
    stack_top = (stack_top & ~static_cast<u64>(0xF)) - sizeof(u64);
    initial_frame(*task, reinterpret_cast<u64>(task_bootstrap), gdt::KERNEL_CODE,
                  stack_top, gdt::KERNEL_DATA);
    preempt_enable();
    return true;
}

bool create_user(const char* name, mm::address_space::AddressSpace& space,
                 u64 entry, u64 user_stack, u32& id) {
    if (!space.root || !mm::address_space::contains(space, entry, 1)
        || !mm::address_space::contains(space, user_stack - 1, 1)) return false;
    preempt_disable();
    Task* task = allocate_task(name, id);
    if (!task) {
        preempt_enable();
        return false;
    }
    task->user_mode = true;
    task->space = space;
    space.root = 0;
    initial_frame(*task, entry, gdt::USER_CODE, user_stack, gdt::USER_DATA);
    preempt_enable();
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
        fpu::save(previous->fpu_state);
        if (previous->state == State::running) previous->state = State::ready;
        if (previous->state != State::exited) previous->frame = frame;
    } else {
        fpu::save(g_boot_fpu);
        g_boot_frame = frame;
    }

    Task* next = next_ready();
    if (!next) {
        g_current = nullptr;
        if (previous && previous->state == State::exited) g_retired_task = previous;
        if ((arch::read_cr3() & 0x000FFFFFFFFFF000ULL) != g_kernel_cr3)
            arch::write_cr3(g_kernel_cr3);
        gdt::set_kernel_stack(g_boot_rsp0);
        fpu::restore(g_boot_fpu);
        return g_boot_frame ? g_boot_frame : frame;
    }

    if (previous && previous->state == State::exited) g_retired_task = previous;

    next->state = State::running;
    ++next->switches;
    g_current = next;
    const paddr_t next_cr3 = next->user_mode ? next->space.root : g_kernel_cr3;
    if ((arch::read_cr3() & 0x000FFFFFFFFFF000ULL) != next_cr3)
        arch::write_cr3(next_cr3);
    const u64 kernel_stack_top = mm::task_stack::top(next->stack);
    gdt::set_kernel_stack(kernel_stack_top);
    fpu::restore(next->fpu_state);
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
    for (usize i = 0; i < g_task_capacity; ++i) {
        Task* task = g_tasks[i];
        if (task && task->active && task->id == id && task->state == State::blocked) {
            task->state = State::ready;
            return;
        }
    }
}

void mark_current_exited() {
    if (g_current) {
        keyboard::clear_waiter(g_current->id);
        g_current->state = State::exited;
    }
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
    for (usize i = 0; i < g_task_capacity; ++i) {
        const Task* task = g_tasks[i];
        if (!task || !task->active) continue;
        if (position-- != 0) continue;
        info = {task->id, task->name, task->state, task->switches};
        return true;
    }
    return false;
}

mm::address_space::AddressSpace* current_address_space() {
    return g_current && g_current->user_mode ? &g_current->space : nullptr;
}

}