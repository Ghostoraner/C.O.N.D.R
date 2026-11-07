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

} // namespace nord::test