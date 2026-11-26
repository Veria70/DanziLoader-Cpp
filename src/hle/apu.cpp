// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio decoder, `xapu.dll` (`victory-hle/src/modules/apu.rs`;
// DanziLoader / victory-perf, see NOTICE.md): Opus packets in, float PCM
// out.
//
// A title connects once for one kind of processing, gets a block of memory
// for its packets and output, and then queues per-stream commands
// (activate, process, reset, deactivate) and dequeues one result per
// command. We decode each command when it is queued, so its result is ready
// at once; the order of results is the order of commands.
//
// The Opus decoder itself is libopus 1.6.1 (third_party/opus), which the
// reference's `opus-pure` reproduces: `decode` is `opus_decode_float`
// without FEC, as `OpusDecoder::decode` is. Debug log lines are not ported
// (a release build logs info and above).
#include <windows.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <opus.h>

#include "core/log.hpp"
#include "modules.hpp"

namespace danzi::hle::apu {

namespace {

constexpr const char* T = "victory_hle::modules::apu";
constexpr HRESULT_ E_OUTOFMEMORY_ = static_cast<HRESULT_>(0x8007000Eu);
constexpr HRESULT_ QUEUE_FULL = static_cast<HRESULT_>(0x8AC81000u);

constexpr HRESULT_ PENDING_RESULTS = static_cast<HRESULT_>(0x8AC81001u);
constexpr HRESULT_ INVALID_COMMAND = static_cast<HRESULT_>(0x8AC81002u);
constexpr HRESULT_ INVALID_STREAM = static_cast<HRESULT_>(0x8AC81003u);
constexpr HRESULT_ ALREADY_ACTIVE = static_cast<HRESULT_>(0x8AC81005u);
constexpr HRESULT_ BAD_PACKET = static_cast<HRESULT_>(0x8AC81008u);


constexpr std::uint32_t DECODE_OPUS = 1;

constexpr std::uint32_t ACTIVATE = 1;
constexpr std::uint32_t DEACTIVATE = 2;
constexpr std::uint32_t PROCESS = 3;
constexpr std::uint32_t NOTIFY = 4;
constexpr std::uint32_t RESET = 5;


constexpr std::uint32_t SIGNAL = 1;

constexpr std::uint32_t DEINTERLEAVED_OUTPUT = 4;
constexpr std::uint32_t RESULT_EVENT = 8;


constexpr std::size_t MAX_FRAMES = 5760;


class Opus {
public:
    static std::optional<Opus> create(std::size_t channels) {
        if (channels != 1 && channels != 2) {
            return std::nullopt;
        }
        int error = OPUS_OK;
        OpusDecoder* d = opus_decoder_create(48000, static_cast<int>(channels), &error);
        if (!d || error != OPUS_OK) {
            return std::nullopt;
        }
        return Opus(d, channels);
    }
    Opus(Opus&& o) noexcept : decoder_(std::exchange(o.decoder_, nullptr)), channels_(o.channels_) {}
    Opus& operator=(Opus&& o) noexcept {
        if (this != &o) {
            if (decoder_) {
                opus_decoder_destroy(decoder_);
            }
            decoder_ = std::exchange(o.decoder_, nullptr);
            channels_ = o.channels_;
        }
        return *this;
    }
    ~Opus() {
        if (decoder_) {
            opus_decoder_destroy(decoder_);
        }
    }
    std::size_t channels() const { return channels_; }
    void reset_state() { opus_decoder_ctl(decoder_, OPUS_RESET_STATE); }


    std::optional<std::size_t> decode(const std::uint8_t* packet, std::size_t len, std::size_t max_frames, float* pcm) {
        if (len > 0x7fffffff) {
            return std::nullopt;
        }
        int n = opus_decode_float(decoder_, packet, static_cast<opus_int32>(len), pcm, static_cast<int>(max_frames), 0);
        if (n < 0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(n);
    }

private:
    Opus(OpusDecoder* d, std::size_t channels) : decoder_(d), channels_(channels) {}
    OpusDecoder* decoder_;
    std::size_t channels_;
};

}   

struct ConnectInput {
    std::uint32_t process;
    std::uint32_t streams;
    std::uint32_t commands_per_stream;
    std::uint32_t memory;
    std::uint32_t frames_per_output;
    std::uint32_t options;
};

struct ConnectOutput {
    void* memory;
    HANDLE result_event;
};

#pragma pack(push, 1)

struct CommandId {
    std::uint32_t kind;
    std::uint32_t stream;
    std::uint32_t sequence;
    std::uint32_t options;
    std::uint64_t context;
};

struct Activate {
    CommandId id;
    std::uint32_t channels;
    std::uint32_t init_length;
    const void* init;
    float start_pitch;
    std::uint32_t processing_length;
    void* processing;
};

struct Process {
    CommandId id;
    float pitch_ramp;
    float target_pitch;
    std::uint32_t first_frame;
    std::uint32_t frame_count;
    std::uint32_t input_length;
    const std::uint8_t* input;
    std::uint32_t output_length;
    std::uint8_t* output;
};

struct ApuResult {
    CommandId id;
    std::uint32_t state;
    std::uint64_t state_data;
    std::uint64_t usec;
    HRESULT_ error;
    std::uint32_t frames;
    std::uint32_t output_length;
    std::uint8_t* output;
    float pitch;
    std::uint32_t input_length;
    const std::uint8_t* input;
};
#pragma pack(pop)
static_assert(sizeof(CommandId) == 24 && sizeof(Activate) == 56 && sizeof(Process) == 64 && sizeof(ApuResult) == 0x50);

namespace {

struct State {
    std::vector<std::optional<Opus>> streams;

    std::vector<std::uint32_t> queued;
    std::deque<ApuResult> results;
    std::uint32_t commands = 0;
    std::uint32_t dequeued = 0;
};

}   

struct Connection {
    std::uint8_t* memory;
    HANDLE event;
    std::uint32_t options;
    std::uint32_t per_stream;
    std::mutex lock;
    State state;
};

HRESULT_ XApuConnect(const ConnectInput* input, ConnectOutput* output, Connection** handle) {
    if (!input || !output || !handle) {
        return E_INVALIDARG_;
    }
    if (input->streams < 1 || input->streams > 500 || input->commands_per_stream < 1 || input->commands_per_stream > 16 || input->memory == 0) {
        return E_INVALIDARG_;
    }
    if (input->process != DECODE_OPUS) {


        log::warn(T, "audio decoder: processing kind not implemented", {{"process", log::debug(input->process)}});
        return E_NOTIMPL_;
    }
    if (input->frames_per_output != 0) {
        return E_INVALIDARG_;
    }


    auto* memory = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, input->memory));
    if (!memory) {
        return E_OUTOFMEMORY_;
    }
    HANDLE event = nullptr;
    if ((input->options & RESULT_EVENT) != 0) {

        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    std::size_t streams = input->streams;
    log::info(T, "audio decoder connected",
              {{"streams", log::debug(static_cast<std::uint64_t>(streams))}, {"memory", log::debug(input->memory)}, {"options", log::debug(input->options)}});
    auto* connection = new Connection{memory, event, input->options, input->commands_per_stream, {}, {}};
    connection->state.streams.resize(streams);
    connection->state.queued.assign(streams, 0);
    *output = ConnectOutput{memory, event};
    *handle = connection;
    return S_OK_;
}

HRESULT_ XApuDisconnect(Connection* handle) {
    if (!handle) {
        return E_INVALIDARG_;
    }

    if (handle->event) {
        CloseHandle(handle->event);
    }
    HeapFree(GetProcessHeap(), 0, handle->memory);
    delete handle;
    return S_OK_;
}

namespace {


void process(Opus& decoder, std::uint32_t options, const Process& command, ApuResult& result) {
    std::size_t channels = decoder.channels();
    const std::uint8_t* input = command.input;
    std::uint32_t input_length = command.input_length;
    std::uint8_t* output = command.output;
    std::uint32_t output_length = command.output_length;
    result.input = input;
    result.input_length = input_length;
    result.output = output;
    if (!input || !output || input_length == 0) {
        result.error = E_INVALIDARG_;
        return;
    }

    std::vector<float> pcm(MAX_FRAMES * channels, 0.0f);
    auto decoded = decoder.decode(input, input_length, MAX_FRAMES, pcm.data());
    if (!decoded) {
        result.error = BAD_PACKET;
        return;
    }
    std::size_t first = std::min<std::size_t>(command.first_frame, *decoded);
    std::size_t frames = std::min({*decoded - first, std::size_t{command.frame_count}, output_length / (4 * channels)});
    const float* kept = pcm.data() + first * channels;

    auto* out = reinterpret_cast<float*>(output);
    if ((options & DEINTERLEAVED_OUTPUT) != 0 && channels == 2) {
        for (std::size_t i = 0; i < frames; ++i) {
            out[i] = kept[2 * i];
            out[frames + i] = kept[2 * i + 1];
        }
    } else if (frames * channels > 0) {
        std::memcpy(out, kept, frames * channels * sizeof(float));
    }
    result.frames = static_cast<std::uint32_t>(frames);
    result.output_length = static_cast<std::uint32_t>(frames * channels * 4);
}

}   

HRESULT_ XApuEnqueueCommand(Connection* handle, const CommandId* command, std::uint32_t* total) {
    if (!handle || !command) {
        return E_INVALIDARG_;
    }

    CommandId id;
    std::memcpy(&id, command, sizeof id);
    std::lock_guard l(handle->lock);
    State& state = handle->state;
    std::size_t stream = id.stream;
    if (stream >= state.streams.size()) {
        return INVALID_STREAM;
    }
    if (state.queued[stream] >= handle->per_stream) {
        return QUEUE_FULL;
    }
    ApuResult result{};
    result.id = id;
    result.error = S_OK_;
    result.pitch = 1.0f;
    auto& decoder = state.streams[stream];
    switch (id.kind) {
        case ACTIVATE: {

            Activate activate;
            std::memcpy(&activate, command, sizeof activate);
            std::uint32_t channels = activate.channels;
            if (decoder) {
                result.error = ALREADY_ACTIVE;
            } else if (auto d = Opus::create(channels); d && channels >= 1 && channels <= 2) {
                decoder = std::move(d);
                result.error = S_OK_;
            } else {
                result.error = E_INVALIDARG_;
            }
            break;
        }
        case DEACTIVATE: decoder.reset(); break;
        case RESET:
            if (decoder) {
                decoder->reset_state();
            }
            break;
        case NOTIFY: break;
        case PROCESS: {

            Process p;
            std::memcpy(&p, command, sizeof p);
            if (decoder) {
                process(*decoder, handle->options, p, result);
            } else {
                result.error = INVALID_STREAM;
            }
            break;
        }
        default: return INVALID_COMMAND;
    }
    state.queued[stream] += 1;
    state.commands += 1;
    state.results.push_back(result);
    if (total) {
        *total = state.commands;
    }
    if ((id.options & SIGNAL) != 0 && handle->event) {
        SetEvent(handle->event);
    }
    return S_OK_;
}

HRESULT_ XApuDequeueResult(Connection* handle, ApuResult* out) {
    if (!handle || !out) {
        return E_INVALIDARG_;
    }
    std::lock_guard l(handle->lock);
    State& state = handle->state;
    if (state.results.empty()) {
        return PENDING_RESULTS;
    }
    ApuResult result = state.results.front();
    state.results.pop_front();
    state.queued[result.id.stream] -= 1;
    state.dequeued += 1;

    std::memcpy(out, &result, sizeof result);
    return S_OK_;
}

std::uint32_t XApuGetResultCount(Connection* handle, std::uint32_t* total) {
    if (!handle) {
        return 0;
    }
    std::lock_guard l(handle->lock);
    if (total) {
        *total = handle->state.dequeued;
    }
    return static_cast<std::uint32_t>(handle->state.results.size());
}

#define E(f) HleExport{#f, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() {
    return {E(XApuConnect), E(XApuDisconnect), E(XApuEnqueueCommand), E(XApuDequeueResult), E(XApuGetResultCount)};
}
#undef E

}   
