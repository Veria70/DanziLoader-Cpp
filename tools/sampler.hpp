// SPDX-License-Identifier: GPL-3.0-or-later
// A sampling profile of the calling thread (a thread of ours suspends it
// every 0.2 ms and notes where it is), printed to stderr by function
// (DbgHelp, with the PDBs beside the binaries). As the title driver's
// `DRIVER_PROFILE`, for tools without it. Nothing that takes the loader's or
// the heap's locks runs while the thread is suspended.
#pragma once

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

class Sampler {
public:


    void start(bool inclusive = false) {
        inclusive_ = inclusive;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        samples_.reserve(inclusive ? 1 << 25 : 1 << 22);
        worker_ = std::thread([this] {
            while (!stop_.load(std::memory_order_relaxed)) {
                if (SuspendThread(target_) != DWORD(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = inclusive_ ? CONTEXT_FULL : CONTEXT_CONTROL;
                    if (GetThreadContext(target_, &c) && samples_.size() + 66 < samples_.capacity()) {
                        if (!inclusive_) {
                            samples_.push_back(c.Rip);
                        } else {
                            for (int depth = 0; depth < 64 && c.Rip; ++depth) {
                                samples_.push_back(c.Rip);
                                DWORD64 base = 0;
                                auto* f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                                if (!f) {
                                    c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                                    c.Rsp += 8;
                                    continue;
                                }
                                void* handler = nullptr;
                                DWORD64 frame = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler, &frame, nullptr);
                            }
                            samples_.push_back(0);
                            ++count_;
                        }
                    }
                    ResumeThread(target_);
                }
                auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
                while (std::chrono::steady_clock::now() < until) {
                    YieldProcessor();
                }
            }
        });
    }

    void report(const char* mode) {
        stop_ = true;
        worker_.join();
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        std::map<std::string, std::uint64_t> by;
        bool addr = mode && std::string(mode) == "addr";
        std::map<DWORD64, std::string> names;
        if (inclusive_) {

            std::vector<std::string> on;


            const char* leaf = std::getenv("SHADER_PROFILE_LEAF");
            for (DWORD64 rip : samples_) {
                if (rip == 0) {
                    if (!leaf || (!on.empty() && on[0].find(leaf) != std::string::npos)) {
                        for (auto& n : on) {
                            ++by[n];
                        }

                        if (leaf) {
                            std::string chain = ">>";
                            for (std::size_t k = 1; k < on.size() && k < 5; ++k) {
                                chain += " < " + on[k].substr(0, 70);
                            }
                            ++by[chain];
                        }
                    }
                    on.clear();
                    continue;
                }
                auto it = names.find(rip);
                if (it == names.end()) {
                    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
                    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 511;
                    DWORD64 disp = 0;
                    it = names.emplace(rip, SymFromAddr(process, rip, &disp, sym) ? std::string(sym->Name) : std::string("?")).first;
                }
                if (std::find(on.begin(), on.end(), it->second) == on.end()) {
                    on.push_back(it->second);
                }
            }
        }
        for (DWORD64 rip : inclusive_ ? std::vector<DWORD64>{} : samples_) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            std::string name = "?";
            if (SymFromAddr(process, rip, &disp, sym)) {
                name = sym->Name;
                if (addr) {
                    char off[32];
                    std::snprintf(off, sizeof off, "+0x%llx", static_cast<unsigned long long>(disp));
                    name += off;
                }
            }
            ++by[name];
        }
        std::vector<std::pair<std::uint64_t, std::string>> sorted;
        for (auto& [n, c] : by) {
            sorted.emplace_back(c, n);
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::size_t total = inclusive_ ? count_ : samples_.size();
        std::fprintf(stderr, "profile: %zu samples\n", total);
        for (std::size_t i = 0; i < sorted.size() && i < (inclusive_ ? 120u : 40u); ++i) {
            std::fprintf(stderr, "%6.2f%%  %s\n", 100.0 * static_cast<double>(sorted[i].first) / static_cast<double>(total),
                         sorted[i].second.c_str());
        }
    }

private:
    HANDLE target_ = nullptr;
    std::atomic<bool> stop_{false};
    std::vector<DWORD64> samples_;
    std::thread worker_;
    bool inclusive_ = false;
    std::size_t count_ = 0;
};
