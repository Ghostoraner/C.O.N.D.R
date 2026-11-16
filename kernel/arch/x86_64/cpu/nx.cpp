#include <arch/x86_64/cpu/nx.hpp>
#include <arch/x86_64/io.hpp>

namespace nord::arch {

static constexpr u32 MSR_EFER = 0xC0000080;
static constexpr u64 EFER_NXE = 1ULL << 11;
static constexpr u32 CPUID_EXTENDED_MAX = 0x80000000;
static constexpr u32 CPUID_EXTENDED_FEATURES = 0x80000001;
static constexpr u32 CPUID_NX = 1U << 20;

static bool g_nx_enabled;

bool init_nx() {
    u32 eax, ebx, ecx, edx;
    cpuid(CPUID_EXTENDED_MAX, 0, eax, ebx, ecx, edx);
    if (eax < CPUID_EXTENDED_FEATURES) return false;

    cpuid(CPUID_EXTENDED_FEATURES, 0, eax, ebx, ecx, edx);
    if (!(edx & CPUID_NX)) return false;

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    g_nx_enabled = true;
    return true;
}

bool nx_enabled() {
    return g_nx_enabled;
}

}
