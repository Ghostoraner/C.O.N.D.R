#include <test/test.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/dev/timer.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <fs/nvmfs.hpp>
#include <ipc/ipc.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <mm/task_stack.hpp>
#include <storage/persistence.hpp>

namespace nord::test {

// Static global variables
static bool g_tests_registered = false;
static usize g_test_count = 0;
static TestInfo g_tests[32];

bool TestSuite::register_test(const char* name, usize iterations, bool (*fn)(usize)) {
    if (g_tests_registered) return false;
    if (g_test_count >= 32) return false;
    TestInfo& info = g_tests[g_test_count];
    info.name = name;
    info.iterations = iterations;
    info.fn = fn;
    info.result = TestResult::skipped;
    info.failures = 0;
    info.total_checks = 0;
    g_test_count++;
    return true;
}

bool TestSuite::run_all() {
    if (g_test_count == 0) return true;
    bool all_passed = true;
    for (usize i = 0; i < g_test_count; ++i) {
        TestInfo& info = g_tests[i];
        info.result = TestResult::passed;
        info.failures = 0;
        info.total_checks = 0;

        kprintf("[test] %s (iter: %u) ... ", info.name, info.iterations);

        for (usize iter = 0; iter < info.iterations; ++iter) {
            if (!info.fn(iter)) {
                info.result = TestResult::failed;
                info.failures++;
                all_passed = false;
                break;
            }
            info.total_checks++;
        }

        if (info.result == TestResult::passed) {
            kprintf("PASSED\n");
        } else {
            kprintf("FAILED\n");
        }
    }
    return all_passed;
}

const TestInfo* TestSuite::get_results() { return g_tests; }
usize TestSuite::get_count() { return g_test_count; }

static bool memory_heap_allocate_deallocate(usize) {
    void* p1 = mm::heap::kmalloc(64);
    if (!p1) return false;
    mm::heap::kfree(p1);
    void* p2 = mm::heap::kmalloc(4096);
    if (!p2) return false;
    mm::heap::kfree(p2);
    return true;
}

static bool memory_pmm_allocate_page(usize) {
    paddr_t p = mm::pmm::alloc_page();
    if (!p) return false;
    mm::pmm::free_page(p);
    return true;
}

static bool memory_pmm_boundaries(usize) {
    if (mm::pmm::alloc_pages(0) != 0) return false;

    const u64 before = mm::pmm::used_bytes();
    const paddr_t pages = mm::pmm::alloc_pages(4);
    if (!pages || (pages & (PAGE_SIZE - 1))) return false;
    for (usize i = 0; i < 4; ++i) {
        auto* page = mm::pmm::phys_to_virt<volatile u64>(pages + i * PAGE_SIZE);
        const u64 pattern = 0xA5A5A5A500000000ULL | i;
        *page = pattern;
        if (*page != pattern) {
            mm::pmm::free_pages(pages, 4);
            return false;
        }
    }
    mm::pmm::free_pages(pages, 4);
    if (mm::pmm::used_bytes() != before) return false;

    if (mm::pmm::alloc_pages(~static_cast<usize>(0))) return false;
    return mm::pmm::used_bytes() == before;
}

static bool memory_heap_boundaries(usize) {
    if (mm::heap::kmalloc(0)) return false;
    if (mm::heap::kmalloc(~static_cast<usize>(0))) return false;
    if (mm::heap::kmalloc(~static_cast<usize>(0) - 1)) return false;
    if (mm::heap::kmalloc(~static_cast<usize>(0) - 15)) return false;
    if (mm::heap::kcalloc(~static_cast<usize>(0) / 2 + 1, 2)) return false;

    u64 used_before = 0;
    u64 capacity = 0;
    mm::heap::stats(used_before, capacity);

    void* small = mm::heap::kmalloc(1);
    void* block = mm::heap::kmalloc(1024);
    void* grown = mm::heap::krealloc(nullptr, 32);
    if (!small || !block || !grown) {
        mm::heap::kfree(small);
        mm::heap::kfree(block);
        mm::heap::kfree(grown);
        return false;
    }
    memset(small, 0xA5, 1);
    memset(block, 0x5A, 1024);
    if (mm::heap::krealloc(grown, 0)) {
        mm::heap::kfree(small);
        mm::heap::kfree(block);
        return false;
    }
    mm::heap::kfree(small);
    mm::heap::kfree(block);

    u64 used_after = 0;
    u64 capacity_after = 0;
    mm::heap::stats(used_after, capacity_after);
    return used_after == used_before && capacity_after >= capacity;
}

static bool ipc_queue_boundaries(usize) {
    u32 checks = 0;
    ipc::Error error;
    return ipc::selftest(checks, error) && checks >= 17;
}

static bool nvmfs_snapshot_corruption(usize) {
    static constexpr char probe_name[] = "snapshot-corruption-probe";
    static constexpr char probe_data[] = "snapshot-corruption-data";
    nvmfs::NodeId id = 0;
    nvmfs::Error error;
    if (!nvmfs::create(nvmfs::NodeType::file, probe_name, 0, id, error)) return false;
    bool ok = nvmfs::write(id, probe_data, sizeof(probe_data) - 1, false, error);

    constexpr usize CAPACITY = 64 * 1024;
    auto* snapshot = static_cast<u8*>(mm::heap::kmalloc(CAPACITY));
    auto* copy = static_cast<u8*>(mm::heap::kmalloc(CAPACITY));
    usize size = 0;
    if (!snapshot || !copy || !nvmfs::serialize(snapshot, CAPACITY, size)
        || size < 32) ok = false;

    if (ok) {
        const usize offsets[] = {0, 4, 8, 12, 16};
        for (usize i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
            memcpy(copy, snapshot, size);
            copy[offsets[i]] ^= 0xFF;
            if (nvmfs::deserialize(copy, size)) { ok = false; break; }
        }
    }
    if (ok && nvmfs::deserialize(snapshot, size - 1)) ok = false;

    if (ok) {
        nvmfs::Lock guard;
        nvmfs::NodeInfo node{};
        ok = nvmfs::find(id, node) && node.stream_size == sizeof(probe_data) - 1
            && memcmp(node.stream, probe_data, sizeof(probe_data) - 1) == 0;
    }

    mm::heap::kfree(snapshot);
    mm::heap::kfree(copy);
    nvmfs::erase(id, error);
    return ok;
}

static bool storage_snapshot_corruption(usize) {
    u32 checks = 0;
    return nord::storage::persistence::selftest(checks) && checks >= 5;
}

static bool memory_address_space_create(usize) {
    mm::address_space::AddressSpace space;
    if (!mm::address_space::create(space)) return false;
    const paddr_t physical = mm::pmm::alloc_page();
    if (!physical) {
        mm::address_space::destroy(space);
        return false;
    }
    if (!mm::address_space::map_page(space, space.user_base + 0x1000, physical, true)) {
        mm::pmm::free_page(physical);
        mm::address_space::destroy(space);
        return false;
    }
    mm::address_space::destroy(space);
    return true;
}

static bool scheduler_switch_count(usize) {
    return nord::arch::scheduler::count() != 0;
}

namespace {

constexpr usize MANY_TASK_COUNT = 20;
volatile u64 g_many_task_done;

void many_task_run() {
    __atomic_add_fetch(&g_many_task_done, 1, __ATOMIC_RELAXED);
    nord::arch::scheduler::exit_current(0);
}

} // namespace

static bool scheduler_many_tasks(usize) {
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::yield;

    if (current_id() == 0) return true;
    g_many_task_done = 0;

    u32 ids[MANY_TASK_COUNT]{};
    usize created = 0;
    while (created < MANY_TASK_COUNT) {
        u32 id = 0;
        if (!create("many-task", &many_task_run, id)) break;
        ids[created++] = id;
    }

    bool ok = created == MANY_TASK_COUNT;
    ok = ok && nord::arch::scheduler::capacity() >= created;
    ok = ok && nord::arch::scheduler::slab_capacity() >= created;
    for (u64 spin = 0; spin < 1000000 && g_many_task_done < created; ++spin) yield();
    ok = ok && g_many_task_done == created;

    for (usize i = 0; i < created; ++i) {
        i32 status = -1;
        u32 child = 0;
        if (!nord::arch::scheduler::wait(static_cast<i32>(ids[i]), status, child)
            || child != ids[i] || status != 0) ok = false;
    }
    return ok;
}

namespace {

constexpr i32 LIFECYCLE_STATUS = 42;

void lifecycle_child() {
    nord::arch::scheduler::exit_current(LIFECYCLE_STATUS);
}

bool lifecycle_cycle(u64& pmm_after) {
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::wait;

    u32 id = 0;
    if (!create("lifecycle-child", &lifecycle_child, id)) return false;
    i32 status = 0;
    u32 child = 0;
    if (!wait(static_cast<i32>(id), status, child)) return false;
    if (child != id || status != LIFECYCLE_STATUS) return false;
    pmm_after = mm::pmm::used_bytes();
    return true;
}

} // namespace

// A child exits with a status that wait() collects. Repeating the cycle must
// leave the task table and the page allocator at the same level, proving the
// PML4, pages and kernel stack were reclaimed.
static bool process_lifecycle(usize) {
    using nord::arch::scheduler::count;
    using nord::arch::scheduler::current_id;

    if (current_id() == 0) return true;
    const usize tasks_before = count();

    // The first cycle may grow the task table and stack page tables; take the
    // baseline after that warm-up so the measured cycles must be leak-free.
    u64 baseline = 0;
    u64 after_first = 0;
    u64 after_second = 0;
    if (!lifecycle_cycle(baseline)) return false;
    if (!lifecycle_cycle(after_first)) return false;
    if (!lifecycle_cycle(after_second)) return false;
    if (count() != tasks_before) return false;
    return baseline == after_first && after_first == after_second;
}

namespace {

constexpr u64 SLEEP_TEST_MS = 30;
volatile u64 g_sleep_start_tick;
volatile u64 g_sleep_wake_tick;

void sleep_test_task() {
    g_sleep_start_tick = nord::arch::timer::ticks();
    nord::arch::scheduler::sleep_ms(SLEEP_TEST_MS);
    g_sleep_wake_tick = nord::arch::timer::ticks();
    nord::arch::scheduler::exit_current(0);
}

} // namespace

// A child blocks in sleep_ms and must be woken by a later PIT tick rather than
// spinning. The parent sleeps past the deadline, then verifies both the
// observed tick gap and the child's collected exit status.
static bool scheduler_sleep_wake(usize) {
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::sleep_ms;
    using nord::arch::scheduler::wait;

    if (current_id() == 0) return true;
    g_sleep_start_tick = 0;
    g_sleep_wake_tick = 0;

    u32 id = 0;
    if (!create("sleep-test", &sleep_test_task, id)) return false;
    sleep_ms(SLEEP_TEST_MS * 2 + 40);

    const u64 frequency = nord::arch::timer::frequency();
    const u64 minimum = frequency ? (SLEEP_TEST_MS * frequency) / 1000 : 1;
    bool ok = g_sleep_start_tick != 0
        && g_sleep_wake_tick >= g_sleep_start_tick + minimum;

    i32 status = -1;
    u32 child = 0;
    if (!wait(static_cast<i32>(id), status, child) || child != id || status != 0) ok = false;
    return ok;
}

namespace {

constexpr char IPC_BLOCK_SERVICE[] = "ipc-block-test";
constexpr u64 IPC_BLOCK_CLIENT_MS = 60;
volatile u64 g_ipc_block_ready;
volatile u64 g_ipc_block_go;
volatile u64 g_ipc_block_start_tick;
volatile u64 g_ipc_block_wake_tick;

void ipc_block_client() {
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::exit_current;
    using nord::arch::scheduler::sleep_ms;
    using nord::arch::scheduler::yield;

    ipc::Handle handle = 0;
    ipc::Error error;
    bool connected = false;
    for (u64 spin = 0; spin < 200000 && !connected; ++spin) {
        connected = ipc::connect(current_id(), IPC_BLOCK_SERVICE, handle, error);
        if (!connected) yield();
    }
    if (!connected) exit_current(1);

    g_ipc_block_ready = 1;
    for (u64 spin = 0; spin < 200000 && !g_ipc_block_go; ++spin) yield();
    if (!g_ipc_block_go) exit_current(2);

    sleep_ms(IPC_BLOCK_CLIENT_MS);
    if (!ipc::send(current_id(), handle, "ping", 4, error)) exit_current(3);
    exit_current(0);
}

void ipc_block_server() {
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::exit_current;
    using nord::arch::scheduler::wait;
    using nord::arch::scheduler::yield;

    const u32 self = current_id();
    nvmfs::NodeId node_id = 0;
    nvmfs::Error fs_error;
    if (!nvmfs::create(nvmfs::NodeType::service, IPC_BLOCK_SERVICE, 0, node_id, fs_error))
        exit_current(1);

    ipc::Handle listener = 0;
    ipc::Error error;
    if (!ipc::register_service(self, IPC_BLOCK_SERVICE, node_id, listener, error)) {
        nvmfs::erase(node_id, fs_error);
        exit_current(2);
    }

    g_ipc_block_ready = 0;
    g_ipc_block_go = 0;
    g_ipc_block_start_tick = 0;
    g_ipc_block_wake_tick = 0;

    u32 client_id = 0;
    if (!create("ipc-block-client", &ipc_block_client, client_id)) exit_current(3);

    for (u64 spin = 0; spin < 200000 && !g_ipc_block_ready; ++spin) yield();
    if (!g_ipc_block_ready) exit_current(4);

    ipc::Handle connection = 0;
    bool accepted = false;
    for (u64 spin = 0; spin < 200000 && !accepted; ++spin) {
        accepted = ipc::accept(self, listener, connection, error);
        if (!accepted) yield();
    }
    if (!accepted) exit_current(5);

    g_ipc_block_go = 1;
    g_ipc_block_start_tick = nord::arch::timer::ticks();
    ipc::Message message{};
    if (!ipc::receive_blocking(self, connection, message, error)) exit_current(6);
    g_ipc_block_wake_tick = nord::arch::timer::ticks();

    const u64 frequency = nord::arch::timer::frequency();
    const u64 minimum = frequency ? (2 * frequency) / 1000 + 1 : 1;
    bool ok = message.size == 4 && memcmp(message.bytes, "ping", 4) == 0
        && g_ipc_block_wake_tick >= g_ipc_block_start_tick + minimum;

    i32 status = -1;
    u32 child = 0;
    if (!wait(static_cast<i32>(client_id), status, child)
        || child != client_id || status != 0) ok = false;
    exit_current(ok ? 0 : 7);
}

} // namespace

// The server registers a service and blocks in ipc_receive; a client wakes it
// by sending a message after sleeping, so the receiver must have transitioned
// to blocked and then been made ready by the IPC send path.
static bool ipc_blocking_receive(usize) {
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::wait;

    if (current_id() == 0) return true;
    u32 id = 0;
    if (!create("ipc-block-server", &ipc_block_server, id)) return false;
    i32 status = -1;
    u32 child = 0;
    if (!wait(static_cast<i32>(id), status, child) || child != id) return false;
    return status == 0;
}

static bool nvmfs_node_capacity(usize) {
    static constexpr usize PROBE_COUNT = 160;
    nvmfs::NodeId ids[PROBE_COUNT];
    nvmfs::Error error;
    usize created = 0;
    while (created < PROBE_COUNT) {
        if (!nvmfs::create(nvmfs::NodeType::file, "capacity-probe", 0,
                           ids[created], error)) break;
        ++created;
    }

    bool ok = created == PROBE_COUNT;
    for (usize i = 0; i < created; ++i) {
        nvmfs::Error erase_error;
        if (!nvmfs::erase(ids[i], erase_error)) ok = false;
    }
    return ok;
}

static bool memory_task_stack_guard(usize) {
    void* first = mm::task_stack::allocate();
    if (!first) return false;
    void* second = mm::task_stack::allocate();
    if (!second) {
        mm::task_stack::release(first);
        return false;
    }

    volatile u64* slot = static_cast<volatile u64*>(first);
    *slot = 0x5A5A5A5A5A5A5A5AULL;
    const bool mapped = *slot == 0x5A5A5A5A5A5A5A5AULL;
    const bool distinct = first != second
        && mm::task_stack::top(first) > reinterpret_cast<u64>(first);

    mm::task_stack::release(second);
    mm::task_stack::release(first);
    return mapped && distinct;
}

namespace {

constexpr usize XMM_TEST_ITERATIONS = 8;
constexpr u64 XMM_TEST_WAIT_YIELDS = 200000;

struct alignas(16) XmmTaskState {
    u8 pattern[16];
    volatile u64 progress;
    volatile u64 mismatches;
    volatile bool done;
    volatile bool stop;
};

XmmTaskState g_xmm_tasks[2];

// Load a pattern into xmm0, let the scheduler switch away and back, then
// capture xmm0. The yield stub itself does not touch SSE registers, so any
// value other than the pattern means the kernel failed to preserve XMM state.
bool xmm_yield_roundtrip(const u8* expected) {
    alignas(16) u8 observed[16];
    void (*const yield_fn)() = &nord::arch::scheduler::yield;
    asm volatile(
        "movdqu (%[pattern]), %%xmm0\n\t"
        "call *%[yield]\n\t"
        "movdqu %%xmm0, (%[observed])\n\t"
        :
        : [pattern] "r"(expected), [observed] "r"(observed), [yield] "r"(yield_fn)
        // The kernel is built with -mno-sse, so the compiler never keeps live
        // values in XMM registers and they need not appear in the clobber list.
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "memory", "cc");
    return memcmp(observed, expected, sizeof(observed)) == 0;
}

void xmm_task_run(XmmTaskState& state) {
    for (usize i = 0; i < XMM_TEST_ITERATIONS; ++i) {
        if (state.stop) break;
        if (!xmm_yield_roundtrip(state.pattern)) state.mismatches = state.mismatches + 1;
        state.progress = state.progress + 1;
    }
    state.done = true;
    nord::arch::scheduler::exit_current(0);
}

void xmm_task_a() { xmm_task_run(g_xmm_tasks[0]); }
void xmm_task_b() { xmm_task_run(g_xmm_tasks[1]); }

} // namespace

static bool fpu_xmm_context_switch(usize) {
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::yield;

    // The test needs the scheduler to be running, otherwise it is skipped.
    if (current_id() == 0) return true;

    static constexpr u8 pattern_a[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    static constexpr u8 pattern_b[16] = {
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};

    XmmTaskState& a = g_xmm_tasks[0];
    XmmTaskState& b = g_xmm_tasks[1];
    memcpy(a.pattern, pattern_a, sizeof(a.pattern));
    memcpy(b.pattern, pattern_b, sizeof(b.pattern));
    a.progress = 0;
    a.mismatches = 0;
    a.done = false;
    a.stop = false;
    b.progress = 0;
    b.mismatches = 0;
    b.done = false;
    b.stop = false;

    u32 id_a = 0;
    u32 id_b = 0;
    if (!create("fpu-xmm-a", &xmm_task_a, id_a)) return false;
    if (!create("fpu-xmm-b", &xmm_task_b, id_b)) {
        a.stop = true;
        for (usize i = 0; i < 64; ++i) yield();
        return false;
    }

    for (u64 spin = 0; spin < XMM_TEST_WAIT_YIELDS && !(a.done && b.done); ++spin) {
        yield();
    }

    const bool completed = a.done && b.done;
    if (!completed) {
        a.stop = true;
        b.stop = true;
    }

    // Collect both children even on timeout so the task table stays clean.
    i32 status_a = -1;
    i32 status_b = -1;
    u32 child_a = 0;
    u32 child_b = 0;
    const bool reaped = nord::arch::scheduler::wait(static_cast<i32>(id_a), status_a, child_a)
        && nord::arch::scheduler::wait(static_cast<i32>(id_b), status_b, child_b)
        && child_a == id_a && child_b == id_b && status_a == 0 && status_b == 0;

    if (!completed) return false;

    return reaped && a.mismatches == 0 && b.mismatches == 0
        && a.progress == XMM_TEST_ITERATIONS && b.progress == XMM_TEST_ITERATIONS;
}

static bool keyboard_queue_order(usize) {
    using nord::arch::keyboard::Event;
    using nord::arch::keyboard::Key;
    using nord::arch::keyboard::test_enqueue;
    using nord::arch::keyboard::try_get_event;

    Event first{Key::character, 'a'};
    Event second{Key::character, 'b'};
    if (!test_enqueue(first)) return false;
    if (!test_enqueue(second)) return false;

    Event out{};
    if (!try_get_event(out) || out.key != Key::character || out.character != 'a') return false;
    if (!try_get_event(out) || out.key != Key::character || out.character != 'b') return false;
    if (try_get_event(out)) return false; // queue must be empty now
    return true;
}

static bool keyboard_full_queue_drops(usize) {
    using nord::arch::keyboard::Event;
    using nord::arch::keyboard::Key;
    using nord::arch::keyboard::test_dropped_count;
    using nord::arch::keyboard::test_enqueue;
    using nord::arch::keyboard::try_get_event;

    const u64 before = test_dropped_count();
    Event event{Key::character, 'x'};

    usize queued = 0;
    while (test_enqueue(event)) ++queued;
    if (queued == 0) return false;

    // The ring keeps one slot free. The enqueue that filled the last slot
    // already counted one drop; another attempt must count one more.
    const u64 after_fill = test_dropped_count();
    if (after_fill != before + 1) return false;
    if (test_enqueue(event)) return false;
    if (test_dropped_count() != after_fill + 1) return false;

    // Drain the queue so later tests start from a clean state.
    while (try_get_event(event)) {}
    return true;
}

static bool keyboard_waiter_list(usize) {
    using nord::arch::keyboard::test_add_waiter;
    using nord::arch::keyboard::test_remove_waiter;
    using nord::arch::keyboard::test_waiter_count;

    // More than the old fixed eight-slot array: every waiter must be accepted.
    constexpr u32 WAITERS = 16;
    u32 ids[WAITERS];
    for (u32 i = 0; i < WAITERS; ++i) ids[i] = 0xF001 + i;

    const u64 before = test_waiter_count();
    bool ok = true;
    for (u32 i = 0; i < WAITERS; ++i) ok = ok && test_add_waiter(ids[i]);
    ok = ok && test_waiter_count() == before + WAITERS;
    ok = ok && test_add_waiter(ids[0]);         // idempotent
    ok = ok && test_waiter_count() == before + WAITERS;

    for (u32 i = 0; i < WAITERS; ++i) test_remove_waiter(ids[i]);
    return ok && test_waiter_count() == before;
}

static bool keyboard_focus_rules(usize) {
    using nord::arch::keyboard::focus_owner;
    using nord::arch::keyboard::release_focus;
    using nord::arch::keyboard::request_focus;

    // A user program holds the keyboard right now (for example kbd-demo):
    // skip, because this test needs the console to own focus.
    if (focus_owner() != 0) return true;

    const u32 a = 0xF101;
    const u32 b = 0xF102;

    bool ok = request_focus(a);
    ok = ok && focus_owner() == a;
    ok = ok && !request_focus(b);      // second owner rejected
    ok = ok && !release_focus(b);      // non-owner cannot release
    ok = ok && release_focus(a);
    ok = ok && focus_owner() == 0;

    if (focus_owner() == a) release_focus(a); // cleanup on failure
    return ok;
}

namespace {

volatile u64 g_kbd_cycle_started;
volatile u64 g_kbd_cycle_char;

void keyboard_cycle_child() {
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::exit_current;

    const u32 self = current_id();
    if (!nord::arch::keyboard::request_focus(self)) exit_current(1);
    g_kbd_cycle_started = 1;

    char character = 0;
    if (!nord::arch::keyboard::read_char_for_task(self, character)) {
        nord::arch::keyboard::release_focus(self);
        exit_current(2);
    }
    g_kbd_cycle_char = static_cast<u8>(character);
    nord::arch::keyboard::release_focus(self);
    exit_current(character == 'z' ? 0 : 3);
}

} // namespace

// Full read_char lifecycle: a child takes keyboard focus, blocks in
// read_char_for_task, an injected IRQ-path event wakes it, and it consumes the
// character before returning focus and exiting.
static bool keyboard_read_char_blocking(usize) {
    using nord::arch::keyboard::Event;
    using nord::arch::keyboard::Key;
    using nord::arch::keyboard::focus_owner;
    using nord::arch::keyboard::test_inject_event;
    using nord::arch::scheduler::create;
    using nord::arch::scheduler::current_id;
    using nord::arch::scheduler::wait;

    if (current_id() == 0) return true;
    if (focus_owner() != 0) return true;

    g_kbd_cycle_started = 0;
    g_kbd_cycle_char = 0;

    u32 id = 0;
    if (!create("kbd-cycle", &keyboard_cycle_child, id)) return false;
    for (u64 spin = 0; spin < 200000 && !g_kbd_cycle_started; ++spin)
        nord::arch::scheduler::yield();

    bool blocked = false;
    for (u64 spin = 0; spin < 200000 && !blocked; ++spin) {
        for (usize i = 0; i < nord::arch::scheduler::count(); ++i) {
            nord::arch::scheduler::TaskInfo info{};
            if (nord::arch::scheduler::at(i, info) && info.id == id
                && info.state == nord::arch::scheduler::State::blocked) {
                blocked = true;
                break;
            }
        }
        if (!blocked) nord::arch::scheduler::yield();
    }

    const bool injected = test_inject_event(Event{Key::character, 'z'});
    i32 status = -1;
    u32 child = 0;
    const bool reaped = wait(static_cast<i32>(id), status, child) && child == id;
    return blocked && injected && reaped && status == 0 && g_kbd_cycle_char == 'z';
}

static bool address_space_copy_invalid(usize) {
    mm::address_space::AddressSpace space;
    if (!mm::address_space::create(space)) return false;
    if (!mm::address_space::allocate_page(space, space.user_base + 0x1000, true)) {
        mm::address_space::destroy(space);
        return false;
    }

    char buffer = 'x';
    const vaddr_t mapped = space.user_base + 0x1000;
    const vaddr_t unmapped = space.user_base + 0x2000;

    // A kernel destination must be rejected.
    if (mm::address_space::copy_to_user(space, reinterpret_cast<vaddr_t>(&buffer), &buffer, 1)) {
        mm::address_space::destroy(space);
        return false;
    }
    // An unmapped user destination must be rejected.
    if (mm::address_space::copy_to_user(space, unmapped, &buffer, 1)) {
        mm::address_space::destroy(space);
        return false;
    }
    // A mapped user destination must succeed.
    if (!mm::address_space::copy_to_user(space, mapped, &buffer, 1)) {
        mm::address_space::destroy(space);
        return false;
    }

    mm::address_space::destroy(space);
    return true;
}

namespace {

// Setting RSP to an invalid value makes the CPU fault while trying to push
// the exception frame onto that same stack. Delivering the second fault on
// the broken stack is impossible, so it escalates to #DF and the IST entry
// takes over. This exercises the same entry path as a real stack overflow,
// but deterministically and without scribbling over adjacent heap memory.
[[noreturn]] __attribute__((naked)) void stack_overflow_trap() {
    __asm__ __volatile__(
        "movq $0, %rsp\n\t"
        "pushq %rax\n\t"
        "ud2\n\t");
}

} // namespace

[[noreturn]] void trigger_kernel_stack_overflow() {
    kprintf("[ist] exhausting the kernel stack...\n");
    stack_overflow_trap();
}

// Auto-registration at static initialization
static bool register_once = []() -> bool {
    TestSuite::register_test("memory_heap_allocate_deallocate", 3, &memory_heap_allocate_deallocate);
    TestSuite::register_test("memory_pmm_allocate_page", 3, &memory_pmm_allocate_page);
    TestSuite::register_test("memory_pmm_boundaries", 2, &memory_pmm_boundaries);
    TestSuite::register_test("memory_heap_boundaries", 2, &memory_heap_boundaries);
    TestSuite::register_test("memory_address_space_create", 3, &memory_address_space_create);
    TestSuite::register_test("ipc_queue_boundaries", 1, &ipc_queue_boundaries);
    TestSuite::register_test("nvmfs_snapshot_corruption", 1, &nvmfs_snapshot_corruption);
    TestSuite::register_test("storage_snapshot_corruption", 1, &storage_snapshot_corruption);
    TestSuite::register_test("scheduler_switch_count", 5, &scheduler_switch_count);
    TestSuite::register_test("scheduler_many_tasks", 1, &scheduler_many_tasks);
    TestSuite::register_test("process_lifecycle", 2, &process_lifecycle);
    TestSuite::register_test("scheduler_sleep_wake", 2, &scheduler_sleep_wake);
    TestSuite::register_test("ipc_blocking_receive", 1, &ipc_blocking_receive);
    TestSuite::register_test("nvmfs_node_capacity", 1, &nvmfs_node_capacity);
    TestSuite::register_test("memory_task_stack_guard", 3, &memory_task_stack_guard);
    TestSuite::register_test("fpu_xmm_context_switch", 1, &fpu_xmm_context_switch);
    TestSuite::register_test("keyboard_queue_order", 5, &keyboard_queue_order);
    TestSuite::register_test("keyboard_full_queue_drops", 3, &keyboard_full_queue_drops);
    TestSuite::register_test("keyboard_waiter_list", 3, &keyboard_waiter_list);
    TestSuite::register_test("keyboard_focus_rules", 3, &keyboard_focus_rules);
    TestSuite::register_test("keyboard_read_char_blocking", 2, &keyboard_read_char_blocking);
    TestSuite::register_test("address_space_copy_invalid", 3, &address_space_copy_invalid);
    g_tests_registered = true;
    return true;
}();

} // namespace nord::test