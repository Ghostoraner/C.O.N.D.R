#include <arch/x86_64/cpu/fpu.hpp>
#include <arch/x86_64/io.hpp>
#include <lib/string.hpp>

namespace nord::arch::fpu {

static constexpr u64 CR0_MP = 1ULL << 1;
static constexpr u64 CR0_EM = 1ULL << 2;
static constexpr u64 CR0_TS = 1ULL << 3;
static constexpr u64 CR4_OSFXSR = 1ULL << 9;
static constexpr u64 CR4_OSXMMEXCPT = 1ULL << 10;

alignas(STATE_ALIGN) static u8 g_initial_state[STATE_SIZE];

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

    // New tasks inherit this template, so clear every XMM register instead of
    // capturing whatever the boot path happened to leave behind.
    asm volatile(
        "pxor %%xmm0, %%xmm0\n\t"
        "pxor %%xmm1, %%xmm1\n\t"
        "pxor %%xmm2, %%xmm2\n\t"
        "pxor %%xmm3, %%xmm3\n\t"
        "pxor %%xmm4, %%xmm4\n\t"
        "pxor %%xmm5, %%xmm5\n\t"
        "pxor %%xmm6, %%xmm6\n\t"
        "pxor %%xmm7, %%xmm7\n\t"
        "pxor %%xmm8, %%xmm8\n\t"
        "pxor %%xmm9, %%xmm9\n\t"
        "pxor %%xmm10, %%xmm10\n\t"
        "pxor %%xmm11, %%xmm11\n\t"
        "pxor %%xmm12, %%xmm12\n\t"
        "pxor %%xmm13, %%xmm13\n\t"
        "pxor %%xmm14, %%xmm14\n\t"
        "pxor %%xmm15, %%xmm15\n\t"
        ::: "memory");

    asm volatile("fxsave64 (%0)" :: "r"(g_initial_state) : "memory");
}

void reset(void* state) {
    memcpy(state, g_initial_state, STATE_SIZE);
}

void save(void* state) {
    asm volatile("fxsave64 (%0)" :: "r"(state) : "memory");
}

void restore(const void* state) {
    asm volatile("fxrstor64 (%0)" :: "r"(state) : "memory");
}

}
