// SPDX-License-Identifier: GPL-3.0-or-later
// `victory precompile`: ahead-of-time shader translation and compute
// pipeline precompilation (`apps/victory/src/precompile.rs`, with
// `crates/victory-gpu/src/precompile.rs`; DanziLoader / victory-perf, see
// NOTICE.md).
//
// Populates the exact same caches that `victory run` uses:
// - `<cache_dir>\shaders\`: translated DXIL + binding metadata for every
//   supported stage (VS, PS, MS, CS), keyed by the same build fingerprint,
//   GPU limits and translation switches.
// - `<cache_dir>\pipelines\host-pipelines.bin` (with `--pipelines`):
//   compiled host D3D12 compute PSOs and GPU driver shader cache entries,
//   built with the exact same root signature and library key as run-time
//   compute pipelines.
//
// Port's choice (as in translate.cpp): the containers of every file are
// picked in file order on this thread (corrupt, not DXIL, filtered,
// duplicate, the limit), translated by the workers a window ahead, and
// their outcomes counted in file order, so the progress lines and the
// first example of each failure are the reference's with one source per
// file.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "core/config.hpp"
#include "d3d12x/host.hpp"
#include "d3d12x/host_heap.hpp"
#include "d3d12x/pipeline.hpp"
#include "d3d12x/shader_cache.hpp"
#include "hle/modules.hpp"
#include "shader/dxbc.hpp"
#include "shader/dxil.hpp"
#include "shader/native.hpp"
#include "shaders.hpp"

namespace danzi::victory {

namespace {

const char* filter_label(StageFilter f) {
    switch (f) {
        case StageFilter::Vs: return "vs";
        case StageFilter::Ps: return "ps";
        case StageFilter::Ms: return "ms";
        case StageFilter::Cs: return "cs";
    }
    return "";
}

bool filter_matches(StageFilter f, shader::HwStage s) {
    using shader::HwStage;
    return (f == StageFilter::Vs && s == HwStage::Vs) || (f == StageFilter::Ps && s == HwStage::Ps) || (f == StageFilter::Ms && s == HwStage::Ms) ||
           (f == StageFilter::Cs && s == HwStage::Cs);
}


const char* stage_name(std::uint32_t stage) {
    static const char* const N[] = {"domain", "hull", "export", "geometry", "vertex", "pixel", "mesh", "compute"};
    return stage < 8 ? N[stage] : "?";
}
constexpr std::uint32_t STAGE_CS = 7;

struct Job {
    std::size_t index = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> file;
    std::vector<std::uint8_t> unpacked;
    std::span<const std::uint8_t> bytes;
    std::string label;
};


struct Outcome {
    const char* stage = nullptr;
    bool cached = false;
    std::optional<std::string> failed;


    std::optional<int> pso;
    std::string refused;
    std::string label;
};


class Semaphore {
public:
    explicit Semaphore(std::size_t limit) : limit_(limit) {}
    template <class F>
    auto run(F f) {
        {
            std::unique_lock l(lock_);
            free_.wait(l, [&] { return active_ < limit_; });
            ++active_;
        }
        struct Guard {
            Semaphore* s;
            ~Guard() {
                {
                    std::lock_guard l(s->lock_);
                    --s->active_;
                }
                s->free_.notify_one();
            }
        } guard{this};
        return f();
    }

private:
    std::size_t limit_, active_ = 0;
    std::mutex lock_;
    std::condition_variable free_;
};

}   

void precompile(std::vector<fs::path> paths_in, DefaultPaths default_paths, bool pipelines, std::optional<StageFilter> only, std::optional<std::size_t> limit,
                std::optional<std::size_t> jobs, std::optional<std::size_t> pipeline_jobs) {
    auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    binary_output();
    auto cfg = config::Config::load();
    auto paths = default_paths(std::move(paths_in));
    fs::path shader_cache_dir = cfg.cache_subdir("shaders");
    fs::path pipeline_cache_dir = shader_cache_dir;
    pipeline_cache_dir.replace_filename(L"pipelines");
    hle::d3d12x_module::use_shader_cache(shader_cache_dir);


    struct HostInfo {
        std::uint32_t view_heap_limit;
        bool mesh_shaders;
    };
    std::optional<HostInfo> host_info;
    if (d3d12x::host()) {
        d3d12x::probe_view_limit();
        host_info = HostInfo{shader::dxil::view_heap_limit(), d3d12x::host_mesh_shaders()};
    } else {
        std::string e = "no host D3D12 device available";
        if (pipelines) {
            throw config::Error({"--pipelines requires a host D3D12 GPU: " + e});
        }
        std::fprintf(stderr, "warning: no host D3D12 device (%s); translating without GPU probe\n", e.c_str());
    }

    std::size_t threads = std::max<std::size_t>(jobs.value_or(available_parallelism()), 1);
    std::size_t max_pipeline_jobs = std::max<std::size_t>(pipeline_jobs.value_or(4), 1);
    std::size_t max_containers = limit.value_or(SIZE_MAX);

    say("shader cache   " + config::display(shader_cache_dir));
    if (pipelines) {
        say(std::format("pipeline cache {} (compute only, up to {} driver compiles at once)", config::display(pipeline_cache_dir), max_pipeline_jobs));
    }
    if (host_info) {
        say(std::format("host GPU       mesh shaders: {}, view heap limit: {}", host_info->mesh_shaders ? "true" : "false", host_info->view_heap_limit));
    }
    if (only) {
        say(std::string("stage filter   ") + filter_label(*only));
    }
    say(std::format("workers        {}", threads));

    std::vector<fs::path> files;
    for (const auto& root : paths) {
        for (auto& entry : walk_files(root)) {
            if (entry.len == 0 || entry.len > MAX_FILE) {
                continue;
            }
            files.push_back(std::move(entry.path));
        }
    }

    struct Totals {
        std::size_t containers = 0, duplicates = 0, skipped_stage = 0, skipped_non_dxil = 0, corrupt = 0, translated = 0, cached = 0;
        std::map<std::string, std::pair<std::size_t, std::size_t>> by_stage;
        std::map<std::string, std::pair<std::size_t, std::string>> failed;
        std::size_t pipelines_compiled = 0, pipelines_loaded = 0;
        std::map<std::string, std::pair<std::size_t, std::string>> pipelines_refused;
    } t;
    std::size_t processed = 0, live_failed = 0;


    Semaphore driver_gate(max_pipeline_jobs);
    std::mutex lock;
    std::condition_variable work, ready;
    std::deque<std::unique_ptr<Job>> queue;
    std::map<std::size_t, Outcome> outcomes;
    bool stop = false;
    auto worker = [&] {
        for (;;) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock l(lock);
                work.wait(l, [&] { return stop || !queue.empty(); });
                if (queue.empty()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            Outcome o;
            o.label = std::move(job->label);


            d3d12x::probe_view_limit();
            d3d12x::Translation tr = d3d12x::host_shader(job->bytes);
            if (!tr.shader) {
                o.failed = std::move(tr.error);
            } else {
                o.stage = stage_name(tr.shader->stage);
                o.cached = tr.cached;
                if (pipelines && tr.shader->stage == STAGE_CS) {


                    o.pso = driver_gate.run([&] { return d3d12x::precompile_compute(*tr.shader, o.label); });
                    if (*o.pso < 0) {
                        o.refused = d3d12x::host() ? "compute pipeline refused" : "no host D3D12 device";
                    }
                }
            }
            {
                std::lock_guard l(lock);
                outcomes[job->index] = std::move(o);
            }
            ready.notify_all();
        }
    };
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    std::size_t submitted = 0, taken = 0;
    auto count = [&](Outcome&& o) {
        if (o.failed) {
            ++live_failed;
            t.failed.try_emplace(*o.failed, 0, o.label).first->second.first += 1;
        } else {
            auto& stage_counts = t.by_stage[o.stage];
            if (o.cached) {
                ++t.cached;
                ++stage_counts.second;
            } else {
                ++t.translated;
                ++stage_counts.first;
            }
            if (o.pso) {
                if (*o.pso == 1) {
                    ++t.pipelines_loaded;
                } else if (*o.pso == 0) {
                    ++t.pipelines_compiled;
                } else {
                    t.pipelines_refused.try_emplace(o.refused, 0, o.label).first->second.first += 1;
                }
            }
        }
        std::size_t done = ++processed;
        if (done % 200 == 0) {
            double secs = elapsed();
            if (pipelines) {
                std::fprintf(stderr, "  %s\n",
                             std::format("{} shaders: {} translated, {} cached, {} failed | compute PSOs: {} compiled, {} loaded ({:.0f} s)", done, t.translated,
                                         t.cached, live_failed, t.pipelines_compiled, t.pipelines_loaded, secs)
                                 .c_str());
            } else {
                std::fprintf(stderr, "  %s\n",
                             std::format("{} shaders: {} translated, {} cached, {} failed ({:.0f} s)", done, t.translated, t.cached, live_failed, secs).c_str());
            }
        }
    };

    auto take = [&](bool wait) -> bool {
        if (taken == submitted) {
            return false;
        }
        Outcome o;
        {
            std::unique_lock l(lock);
            if (wait) {
                ready.wait(l, [&] { return outcomes.contains(taken); });
            }
            auto it = outcomes.find(taken);
            if (it == outcomes.end()) {
                return false;
            }
            o = std::move(it->second);
            outcomes.erase(it);
        }
        ++taken;
        count(std::move(o));
        return true;
    };
    const std::size_t window = std::max<std::size_t>(64, threads * 16);

    std::unordered_set<std::string> seen;
    std::size_t unique_claimed = 0;
    for (const auto& file : files) {
        if (unique_claimed >= max_containers) {
            break;
        }
        auto read = read_whole(file);
        if (!read) {
            continue;
        }
        auto data = std::make_shared<const std::vector<std::uint8_t>>(std::move(*read));
        auto srcs = sources(*data);
        if (srcs.empty()) {
            continue;
        }
        std::string shown = config::display(file);
        for (const auto& src : srcs) {
            if (unique_claimed >= max_containers) {
                break;
            }
            auto job = std::make_unique<Job>();
            job->file = data;
            auto bytes = src.bytes(job->unpacked);
            if (!bytes) {
                ++t.corrupt;
                continue;
            }
            job->bytes = *bytes;
            std::optional<shader::Container> container;
            try {
                container = shader::Container::parse(job->bytes);
            } catch (const std::exception&) {
            }
            if (!container) {
                ++t.skipped_non_dxil;
                continue;
            }
            const shader::ContainerPart* dxil = container->part(shader::fourcc("DXIL"));
            if (!dxil) {
                ++t.skipped_non_dxil;
                continue;
            }
            if (only) {
                bool matches = false;
                if (auto progs = shader::native_programs(dxil->data)) {
                    matches = std::any_of(progs->begin(), progs->end(), [&](const shader::NativeProgram& p) { return filter_matches(*only, p.stage); });
                }
                if (!matches) {
                    ++t.skipped_stage;
                    continue;
                }
            }


            auto full = blake3_of(job->bytes);
            if (!seen.insert(std::string(reinterpret_cast<const char*>(full.data()), full.size())).second) {
                ++t.duplicates;
                continue;
            }
            if (unique_claimed++ >= max_containers) {
                break;
            }
            ++t.containers;
            job->label = shown + src.label() + " #" + hex_prefix(full, 16);
            job->index = submitted++;
            {
                std::lock_guard l(lock);
                queue.push_back(std::move(job));
            }
            work.notify_one();
            while (take(submitted - taken >= window)) {
            }
        }
    }
    while (take(true)) {
    }
    {
        std::lock_guard l(lock);
        stop = true;
    }
    work.notify_all();
    for (auto& w : workers) {
        w.join();
    }

    if (pipelines) {
        d3d12x::save_pipeline_cache();
    }

    double secs = elapsed();
    std::size_t failed_count = 0;
    for (const auto& [r, v] : t.failed) {
        failed_count += v.first;
    }
    say(std::format("containers     {} unique ({} duplicates skipped, {} non-DXIL, {} filtered, {} corrupt)", t.containers, t.duplicates, t.skipped_non_dxil,
                    t.skipped_stage, t.corrupt));
    say(std::format("shaders        {} translated, {} already cached, {} failed ({:.1f} s)", t.translated, t.cached, failed_count, secs));
    for (const auto& [stage, n] : t.by_stage) {
        say(std::format("  {:<12} {} translated, {} cached", stage, n.first, n.second));
    }
    if (pipelines) {
        std::size_t refused = 0;
        for (const auto& [r, v] : t.pipelines_refused) {
            refused += v.first;
        }
        say(std::format("compute PSOs   {} compiled, {} already in library, {} refused", t.pipelines_compiled, t.pipelines_loaded, refused));
    }
    auto top = [](const std::map<std::string, std::pair<std::size_t, std::string>>& m, const char* head, std::size_t n) {
        std::vector<std::pair<std::string, std::pair<std::size_t, std::string>>> v(m.begin(), m.end());
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        if (v.empty()) {
            return;
        }
        say(head);
        for (std::size_t i = 0; i < v.size() && i < n; ++i) {
            say(std::format("  {:>7}  {}  (e.g. {})", v[i].second.first, v[i].first, v[i].second.second));
        }
    };
    top(t.failed, "translation failures (top 15):", 15);
    top(t.pipelines_refused, "compute pipeline refusals (top 10):", 10);
}

}   
