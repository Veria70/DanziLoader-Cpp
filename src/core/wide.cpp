// SPDX-License-Identifier: GPL-3.0-or-later
#include "wide.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <string>
#include <thread>

#include "env.hpp"

#pragma comment(lib, "Synchronization.lib")

namespace danzi::wide {

namespace {


struct Batch {
    std::vector<Job>* jobs = nullptr;   
    usize count = 0;
    std::atomic<usize> next{0};
    std::atomic<usize> left{0};
};


struct Pool {
    SRWLOCK lock = SRWLOCK_INIT;   
    std::shared_ptr<Batch> batch;
    std::atomic<std::uint64_t> generation{0};


    static void work(Batch& b) {
        for (;;) {
            usize i = b.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= b.count) {
                return;
            }
            (*b.jobs)[i]();
            if (b.left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                WakeByAddressAll(&b.left);
            }
        }
    }

    void loop() {
        std::uint64_t seen = 0;
        for (;;) {
            std::uint64_t now = generation.load(std::memory_order_acquire);
            while (now == seen) {
                WaitOnAddress(&generation, &seen, sizeof(seen), INFINITE);
                now = generation.load(std::memory_order_acquire);
            }
            seen = now;
            std::shared_ptr<Batch> b;
            AcquireSRWLockShared(&lock);
            b = batch;
            ReleaseSRWLockShared(&lock);
            if (b) {
                work(*b);
            }
        }
    }
};

Pool& pool() {
    static Pool* p = [] {
        auto* made = new Pool;
        for (usize i = 0, n = threads(); i < n; ++i) {
            std::thread([made] { made->loop(); }).detach();
        }
        return made;
    }();
    return *p;
}


SRWLOCK batches = SRWLOCK_INIT;

}   

usize threads() {


    static const usize n = [] {
        if (auto v = env::get(L"VICTORY_WIDE_THREADS")) {
            try {
                return std::clamp<usize>(std::stoul(*v), 1, 64);
            } catch (...) {
            }
        }
        DWORD_PTR process = 0, system = 0;
        usize cpus = GetProcessAffinityMask(GetCurrentProcess(), &process, &system) ? static_cast<usize>(std::popcount(process)) : 0;
        if (cpus == 0) {
            cpus = std::thread::hardware_concurrency();
        }
        return std::clamp<usize>(cpus / 2, 2, 16);
    }();
    return n;
}

void run(std::vector<Job>& jobs) {
    if (jobs.size() < 2) {
        for (auto& job : jobs) {
            job();
        }
        return;
    }
    Pool& p = pool();
    AcquireSRWLockExclusive(&batches);
    auto b = std::make_shared<Batch>();
    b->jobs = &jobs;
    b->count = jobs.size();
    b->left.store(jobs.size(), std::memory_order_relaxed);
    AcquireSRWLockExclusive(&p.lock);
    p.batch = b;
    ReleaseSRWLockExclusive(&p.lock);
    p.generation.fetch_add(1, std::memory_order_acq_rel);
    WakeByAddressAll(&p.generation);
    Pool::work(*b);
    for (usize left = b->left.load(std::memory_order_acquire); left != 0; left = b->left.load(std::memory_order_acquire)) {
        WaitOnAddress(&b->left, &left, sizeof(left), INFINITE);
    }
    AcquireSRWLockExclusive(&p.lock);
    p.batch.reset();
    ReleaseSRWLockExclusive(&p.lock);
    ReleaseSRWLockExclusive(&batches);
}

}   
