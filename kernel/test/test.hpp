#pragma once
#include <nord/types.hpp>

namespace nord::test {

enum class TestResult : u8 {
    passed,
    failed,
    skipped
};

struct TestInfo {
    const char* name;
    usize       iterations;
    bool      (*fn)(usize); // Змінено тип аргументу на usize для узгодженості
    TestResult  result;
    u64         failures;
    u64         total_checks; // Крапка з комою на місці
};

class TestSuite {
public:
    static bool register_test(const char* name, usize iterations, bool (*fn)(usize));
    static bool run_all();
    static const TestInfo* get_results();
    static usize get_count();
};

// Deliberately runs off the end of the kernel stack so the CPU raises a
// double fault through the IST handler. Never returns.
[[noreturn]] void trigger_kernel_stack_overflow();

} // namespace nord::test