#include <arch/x86_64/cpu/scheduler.hpp>
#include <arch/x86_64/cpu/fpu.hpp>
#include <arch/x86_64/cpu/gdt.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/dev/timer.hpp>
#include <arch/x86_64/io.hpp>
#include <ipc/ipc.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <mm/heap.hpp>
#include <mm/task_stack.hpp>

namespace nord::arch::scheduler {

static constexpr usize NAME_SIZE = 24;
static constexpr usize DEFAULT_STACK_SIZE = 16 * 1024;

struct Task {
    u32 id;
    u32 parent;
    i32 status;
    bool active;
    bool resources_released;
    State state;
    Entry entry;
    Registers* frame;
    alignas(fpu::STATE_ALIGN) u8 fpu_state[fpu::STATE_SIZE];
    void* stack;
    bool user_mode;
    mm::address_space::AddressSpace space;
    u64 switches;
    u64 wake_tick;
    Task* free_next;
    char name[NAME_SIZE];
};

// Task records are carved out of heap-backed slabs. Freed records stay on an
// intrusive free list, so steady-state create/exit cycles do not touch the
// heap after the first slab is allocated.
struct TaskSlab {
    TaskSlab* next;
    usize capacity;
};

static Config g_config;
static TaskSlab* g_slabs;
static Task* g_free_task;
static usize g_slab_capacity;
static Task** g_tasks;
static usize g_task_capacity;
static usize g_count;
static u32 g_next_id;
static Task* g_current;
static Registers* g_boot_frame;
alignas(fpu::STATE_ALIGN) static u8 g_boot_fpu[fpu::STATE_SIZE];
static Task* g_retired_task;
static bool g_running;
static bool g_initialized;
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

static Task* allocate_task_record() {
    if (!g_free_task) {
        const usize count = g_config.tasks_per_slab;
        const usize offset = align_up(sizeof(TaskSlab), alignof(Task));
        if (count > (~static_cast<usize>(0) - offset) / sizeof(Task)) return nullptr;
        auto* slab = static_cast<TaskSlab*>(
            mm::heap::kmalloc(offset + count * sizeof(Task)));
        if (!slab) return nullptr;
        slab->next = g_slabs;
        slab->capacity = count;
        g_slabs = slab;

        auto* objects = reinterpret_cast<Task*>(reinterpret_cast<u8*>(slab) + offset);
        Task* head = g_free_task;
        for (usize i = 0; i < count; ++i) {
            objects[i].free_next = head;
            head = &objects[i];
        }
        g_free_task = head;
        g_slab_capacity += count;
    }
    Task* task = g_free_task;
    g_free_task = task->free_next;
    return task;
}

static void release_task_record(Task* task) {
    task->free_next = g_free_task;
    g_free_task = task;
}

static bool grow_tasks(usize needed) {
    if (needed <= g_task_capacity) return true;
    usize capacity = g_task_capacity ? g_task_capacity : g_config.initial_capacity;
    if (!capacity) capacity = 1;
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
    g_free_task = nullptr;
    while (g_slabs) {
        TaskSlab* next = g_slabs->next;
        mm::heap::kfree(g_slabs);
        g_slabs = next;
    }
    g_slab_capacity = 0;
    if (g_tasks) mm::heap::kfree(g_tasks);
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
    exit_current(0);
}

static Task* allocate_task(const char* name, u32& id) {
    if (!name || !*name) return nullptr;
    usize slot = 0;
    if (!reserve_task_slot(slot)) return nullptr;

    void* stack = mm::task_stack::allocate();
    if (!stack) return nullptr;
    Task* task = allocate_task_record();
    if (!task) {
        mm::task_stack::release(stack);
        return nullptr;
    }

    memset(task, 0, sizeof(*task));
    task->id = g_next_id++;
    task->parent = g_current ? g_current->id : 0;
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

static Task* find_task(u32 id) {
    for (usize i = 0; i < g_task_capacity; ++i) {
        if (g_tasks[i] && g_tasks[i]->id == id) return g_tasks[i];
    }
    return nullptr;
}

// The kernel stack cannot be unmapped while schedule() is still running on
// it, so a zombie's resources are retired on the next scheduling round.
static void release_resources(Task* task) {
    if (task->resources_released) return;
    task->resources_released = true;
    ipc::close_task(task->id);
    // A task that exited while blocked in a keyboard read must not leave a
    // stale waiter registration behind.
    keyboard::clear_waiter(task->id);
    if (task->user_mode) mm::address_space::destroy(task->space);
    if (task->stack) mm::task_stack::release(task->stack);
    task->stack = nullptr;
}

static void destroy_task(Task* task);

static bool waitable(const Task* task) {
    if (!task->parent) return false;
    Task* parent = find_task(task->parent);
    return parent && parent->state != State::zombie;
}

static void destroy_task(Task* task) {
    release_resources(task);
    // Children of a dying task can never be waited for. Reparent them to the
    // reaper and release any zombie that is already collectable.
    for (usize i = 0; i < g_task_capacity; ++i) {
        Task* child = g_tasks[i];
        if (!child || child == task || child->parent != task->id) continue;
        child->parent = 0;
        if (child->state == State::zombie && child->resources_released) destroy_task(child);
    }
    for (usize i = 0; i < g_task_capacity; ++i) {
        if (g_tasks[i] == task) {
            g_tasks[i] = nullptr;
            break;
        }
    }
    release_task_record(task);
    --g_count;
}

static void retire_retired_task() {
    if (!g_retired_task) return;
    Task* task = g_retired_task;
    g_retired_task = nullptr;
    release_resources(task);
    // Tasks without a living parent are not observable through wait(), so
    // their zombie record is reclaimed immediately.
    if (!waitable(task)) destroy_task(task);
}

void configure(const Config& config) {
    if (g_running || g_initialized || g_task_capacity) return;
    g_config = config;
    if (!g_config.initial_capacity) g_config.initial_capacity = 1;
    if (!g_config.tasks_per_slab) g_config.tasks_per_slab = 1;
    if (!g_config.stack_size) g_config.stack_size = DEFAULT_STACK_SIZE;
}

void init() {
    if (!g_config.initial_capacity) g_config.initial_capacity = 8;
    if (!g_config.tasks_per_slab) g_config.tasks_per_slab = 8;
    if (!g_config.stack_size) g_config.stack_size = DEFAULT_STACK_SIZE;

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
    mm::task_stack::init(g_config.stack_size);
    g_initialized = true;
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

static void wake_sleepers() {
    const u64 now = timer::ticks();
    for (usize i = 0; i < g_task_capacity; ++i) {
        Task* task = g_tasks[i];
        if (!task || !task->active || task->state != State::blocked || !task->wake_tick)
            continue;
        if (task->wake_tick <= now) {
            task->wake_tick = 0;
            task->state = State::ready;
        }
    }
}

Registers* schedule(Registers* frame) {
    if (!g_running || g_preempt_depth) return frame;

    wake_sleepers();
    retire_retired_task();

    Task* previous = g_current;
    if (previous) {
        fpu::save(previous->fpu_state);
        if (previous->state == State::running) previous->state = State::ready;
        if (previous->state != State::zombie) previous->frame = frame;
    } else {
        fpu::save(g_boot_fpu);
        g_boot_frame = frame;
    }

    Task* next = next_ready();
    if (!next) {
        g_current = nullptr;
        if (previous && previous->state == State::zombie) g_retired_task = previous;
        if ((arch::read_cr3() & 0x000FFFFFFFFFF000ULL) != g_kernel_cr3)
            arch::write_cr3(g_kernel_cr3);
        gdt::set_kernel_stack(g_boot_rsp0);
        fpu::restore(g_boot_fpu);
        return g_boot_frame ? g_boot_frame : frame;
    }

    if (previous && previous->state == State::zombie) g_retired_task = previous;

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
            task->wake_tick = 0;
            task->state = State::ready;
            return;
        }
    }
}

// Sleep is implemented as a timer wakeup: the task blocks and the scheduler
// scans for expired deadlines on every PIT tick.
void sleep_ms(u64 milliseconds) {
    if (!g_running || !g_current || !milliseconds) {
        if (g_running) yield();
        return;
    }
    const u64 frequency = timer::frequency();
    u64 ticks = 0;
    const u64 max_ms = frequency ? (~static_cast<u64>(0) - 999) / frequency : 0;
    if (frequency) {
        ticks = milliseconds > max_ms
            ? (~static_cast<u64>(0) / frequency)
            : (milliseconds * frequency + 999) / 1000;
    }
    if (!ticks) {
        yield();
        return;
    }

    InterruptGuard guard;
    const u64 now = timer::ticks();
    const u64 deadline = now + ticks;
    g_current->wake_tick = deadline < now ? ~static_cast<u64>(0) : deadline;
    g_current->state = State::blocked;
    isr_yield();
}

static void become_zombie(i32 status) {
    if (!g_current) return;
    keyboard::clear_waiter(g_current->id);
    g_current->status = status;
    g_current->wake_tick = 0;
    g_current->state = State::zombie;
    if (g_current->parent) wake(g_current->parent);
}

void mark_current_exited(i32 status) {
    become_zombie(status);
}

[[noreturn]] void exit_current(i32 status) {
    become_zombie(status);
    isr_yield();
    halt_forever();
}

// Blocks the caller until one of its children becomes a zombie, then reclaims
// the child's PML4, pages, kernel stack and task record. `pid > 0` selects a
// specific child; `pid <= 0` waits for any child.
bool wait(i32 pid, i32& status, u32& child_id) {
    if (!g_running || !g_current) return false;
    const u32 parent = g_current->id;
    for (;;) {
        // Interrupts stay disabled while scanning and blocking, otherwise a
        // child could exit between the scan and the blocking switch and the
        // wakeup would be lost.
        InterruptGuard guard;
        bool has_child = false;
        Task* zombie = nullptr;
        for (usize i = 0; i < g_task_capacity; ++i) {
            Task* task = g_tasks[i];
            if (!task || task->parent != parent) continue;
            if (pid > 0 && task->id != static_cast<u32>(pid)) continue;
            has_child = true;
            if (task->state == State::zombie) {
                zombie = task;
                break;
            }
        }
        if (!has_child) return false;
        if (zombie) {
            status = zombie->status;
            child_id = zombie->id;
            // The zombie may not have been retired yet; its stack is no longer
            // in use, so it can be reclaimed right here.
            if (g_retired_task == zombie) {
                release_resources(zombie);
                g_retired_task = nullptr;
            }
            destroy_task(zombie);
            return true;
        }
        g_current->state = State::blocked;
        isr_yield();
    }
}

[[noreturn]] void run() {
    g_running = true;
    yield();
    for (;;) hlt();
}

usize count() {
    return g_count;
}

usize capacity() {
    return g_task_capacity;
}

usize slab_capacity() {
    return g_slab_capacity;
}

u32 current_id() {
    return g_current ? g_current->id : 0;
}

bool at(usize position, TaskInfo& info) {
    for (usize i = 0; i < g_task_capacity; ++i) {
        const Task* task = g_tasks[i];
        if (!task || !task->active) continue;
        if (position-- != 0) continue;
        info = {task->id, task->parent, task->status, task->name, task->state, task->switches};
        return true;
    }
    return false;
}

mm::address_space::AddressSpace* current_address_space() {
    return g_current && g_current->user_mode ? &g_current->space : nullptr;
}

}