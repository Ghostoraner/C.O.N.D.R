#pragma once
#include <arch/x86_64/cpu/scheduler.hpp>
#include <nord/types.hpp>

namespace nord::sync {

// Single-CPU spinlock. Preemption is disabled for as long as the lock is
// held, so the owner cannot be descheduled and no other task can spin on the
// same lock forever. Interrupt handlers must not take these locks unless they
// run with interrupts disabled and never block.
class SpinLock {
public:
    constexpr SpinLock() : g_locked{0} {}
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() {
        arch::scheduler::preempt_disable();
        while (__atomic_exchange_n(&g_locked, static_cast<u8>(1), __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&g_locked, __ATOMIC_RELAXED))
                asm volatile("pause" ::: "memory");
        }
    }

    void unlock() {
        __atomic_store_n(&g_locked, static_cast<u8>(0), __ATOMIC_RELEASE);
        arch::scheduler::preempt_enable();
    }

private:
    u8 g_locked;
};

// Recursive variant used by subsystems that expose calls to each other (for
// example a caller holding the NVMFS lock while invoking another NVMFS API).
class RecursiveSpinLock {
public:
    constexpr RecursiveSpinLock() : g_locked{0}, g_owner{0}, g_depth{0} {}
    RecursiveSpinLock(const RecursiveSpinLock&) = delete;
    RecursiveSpinLock& operator=(const RecursiveSpinLock&) = delete;

    void lock() {
        const u32 owner = arch::scheduler::current_id();
        if (g_locked && g_owner == owner) {
            ++g_depth;
            return;
        }
        arch::scheduler::preempt_disable();
        while (__atomic_exchange_n(&g_locked, static_cast<u8>(1), __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&g_locked, __ATOMIC_RELAXED))
                asm volatile("pause" ::: "memory");
        }
        g_owner = owner;
        g_depth = 1;
    }

    void unlock() {
        const u32 owner = arch::scheduler::current_id();
        if (!g_locked || g_owner != owner) return;
        if (--g_depth) return;
        g_owner = 0;
        __atomic_store_n(&g_locked, static_cast<u8>(0), __ATOMIC_RELEASE);
        arch::scheduler::preempt_enable();
    }

private:
    u8 g_locked;
    u32 g_owner;
    u32 g_depth;
};

template <typename LockType>
class LockGuard {
public:
    explicit LockGuard(LockType& lock) : g_lock(lock) { g_lock.lock(); }
    ~LockGuard() { g_lock.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    LockType& g_lock;
};

}
