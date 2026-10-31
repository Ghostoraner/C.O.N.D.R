#include <nord/types.hpp>
#include <lib/print.hpp>

extern "C" {

int  __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void __cxa_pure_virtual() { nord::panic("pure virtual function called"); }
void* __dso_handle = nullptr;

int  __cxa_guard_acquire(nord::u64* g) { return !*reinterpret_cast<nord::u8*>(g); }
void __cxa_guard_release(nord::u64* g) { *reinterpret_cast<nord::u8*>(g) = 1; }
void __cxa_guard_abort(nord::u64*) {}

nord::u64 __stack_chk_guard = 0x595e9fbd94fda766ULL;
[[noreturn]] void __stack_chk_fail() { nord::panic("stack smashing detected"); }

}