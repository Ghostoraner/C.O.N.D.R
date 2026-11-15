#include <test/test.hpp>
#include <arch/x86_64/dev/keyboard.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>
#include <fs/nvmfs.hpp>
#include <lib/print.hpp>
#include <lib/string.hpp>
#include <mm/address_space.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>

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
    nord::arch::scheduler::exit_current();
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

    if (!(a.done && b.done)) {
        a.stop = true;
        b.stop = true;
        for (usize i = 0; i < 64; ++i) yield();
        return false;
    }

    return a.mismatches == 0 && b.mismatches == 0
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

    const u64 before = test_waiter_count();
    const u32 a = 0xF001;
    const u32 b = 0xF002;

    bool ok = test_add_waiter(a) && test_add_waiter(b);
    ok = ok && test_waiter_count() == before + 2;
    ok = ok && test_add_waiter(a);              // idempotent
    ok = ok && test_waiter_count() == before + 2;

    test_remove_waiter(a);
    test_remove_waiter(b);
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

// Auto-registration at static initialization
static bool register_once = []() -> bool {
    TestSuite::register_test("memory_heap_allocate_deallocate", 3, &memory_heap_allocate_deallocate);
    TestSuite::register_test("memory_pmm_allocate_page", 3, &memory_pmm_allocate_page);
    TestSuite::register_test("memory_address_space_create", 3, &memory_address_space_create);
    TestSuite::register_test("scheduler_switch_count", 5, &scheduler_switch_count);
    TestSuite::register_test("fpu_xmm_context_switch", 1, &fpu_xmm_context_switch);
    TestSuite::register_test("keyboard_queue_order", 5, &keyboard_queue_order);
    TestSuite::register_test("keyboard_full_queue_drops", 3, &keyboard_full_queue_drops);
    TestSuite::register_test("keyboard_waiter_list", 3, &keyboard_waiter_list);
    TestSuite::register_test("keyboard_focus_rules", 3, &keyboard_focus_rules);
    TestSuite::register_test("address_space_copy_invalid", 3, &address_space_copy_invalid);
    g_tests_registered = true;
    return true;
}();

} // namespace nord::test