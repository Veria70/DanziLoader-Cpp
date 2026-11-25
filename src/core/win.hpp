// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace danzi {

using usize = std::size_t;

inline constexpr usize saturating_add(usize a, usize b) {
    return a > std::numeric_limits<usize>::max() - b ? std::numeric_limits<usize>::max() : a + b;
}

inline constexpr usize saturating_sub(usize a, usize b) { return a > b ? a - b : 0; }


inline constexpr usize div_ceil(usize a, usize b) { return a / b + (a % b != 0 ? 1 : 0); }


class SrwLock {
public:
    SrwLock() = default;
    SrwLock(const SrwLock&) = delete;
    SrwLock& operator=(const SrwLock&) = delete;
    void lock() { AcquireSRWLockExclusive(&l_); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&l_) != 0; }
    void unlock() { ReleaseSRWLockExclusive(&l_); }
    void lock_shared() { AcquireSRWLockShared(&l_); }
    bool try_lock_shared() { return TryAcquireSRWLockShared(&l_) != 0; }
    void unlock_shared() { ReleaseSRWLockShared(&l_); }

private:
    SRWLOCK l_ = SRWLOCK_INIT;
};

#pragma comment(lib, "synchronization.lib")


class FastLock {
public:
    FastLock() = default;
    FastLock(const FastLock&) = delete;
    FastLock& operator=(const FastLock&) = delete;
    void lock() {
        if (InterlockedCompareExchange(&state_, 1, 0) != 0) {
            slow();
        }
    }
    bool try_lock() { return InterlockedCompareExchange(&state_, 1, 0) == 0; }
    void unlock() {
        if (InterlockedExchange(&state_, 0) == 2) {
            WakeByAddressSingle(const_cast<long*>(&state_));
        }
    }

private:
    void slow() {
        while (InterlockedExchange(&state_, 2) != 0) {
            long two = 2;
            WaitOnAddress(&state_, &two, sizeof two, INFINITE);
        }
    }
    volatile long state_ = 0;
};

}   
