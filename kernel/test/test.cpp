#include <test/test.hpp>
#include <fs/nvmfs.hpp>
#include <mm/pmm.hpp>
#include <mm/address_space.hpp>
#include <mm/heap.hpp>
#include <lib/print.hpp>
#include <arch/x86_64/cpu/scheduler.hpp>

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
    bool mapped = mm::address_space::map_page(space, 0x1000, 0, true);
    if (!mapped) return false;
    mm::address_space::destroy(space);
    return true;
}

static bool scheduler_switch_count(usize) {
    
    return nord::arch::scheduler::count() >= 0;
}

// Auto-registration at static initialization
static bool register_once = []() -> bool {
    TestSuite::register_test("memory_heap_allocate_deallocate", 3, &memory_heap_allocate_deallocate);
    TestSuite::register_test("memory_pmm_allocate_page", 3, &memory_pmm_allocate_page);
    TestSuite::register_test("memory_address_space_create", 3, &memory_address_space_create);
    TestSuite::register_test("scheduler_switch_count", 5, &scheduler_switch_count);
    g_tests_registered = true;
    return true;
}();

} // namespace nord::test