#include <arch/x86_64/cpu/fpu.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/string.hpp>

namespace nord::arch::fpu {

static constexpr u64 CR0_MP = 1ULL << 1;
static constexpr u64 CR0_EM = 1ULL << 2;
static constexpr u64 CR0_TS = 1ULL << 3;
static constexpr u64 CR4_OSFXSR = 1ULL << 9;
static constexpr u64 CR4_OSXMMEXCPT = 1ULL << 10;

void init() {
    u64 cr0 = read_cr0();
    cr0 |= CR0_MP;
    cr0 &= ~(CR0_EM | CR0_TS);
    write_cr0(cr0);

    u64 cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);

    asm volatile("fninit" ::: "memory");
    constexpr u32 mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
}

void reset(void* state) {
    auto* bytes = static_cast<u8*>(state);
    memset(bytes, 0, STATE_SIZE);
    constexpr u16 control_word = 0x037F;
    memcpy(bytes, &control_word, sizeof(control_word));
    bytes[4] = 0xFF;
    constexpr u32 mxcsr = 0x1F80;
    memcpy(bytes + 24, &mxcsr, sizeof(mxcsr));
}

void save(void* state) {
    asm volatile("fxsave64 (%0)" :: "r"(state) : "memory");
}

void restore(const void* state) {
    asm volatile("fxrstor64 (%0)" :: "r"(state) : "memory");
}

}
