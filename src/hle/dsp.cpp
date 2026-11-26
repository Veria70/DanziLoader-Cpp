// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio DSP, `chad.dll` (`victory-hle/src/modules/dsp.rs`;
// DanziLoader / victory-perf, see NOTICE.md): streams that convolve blocks
// of float samples with an impulse response (reverb), or run other block
// transforms, on a coprocessor.
//
// A title connects once with a memory block for the DSP, activates streams,
// and submits one command per block (gains, an input and an output buffer).
// Activation hands back a status block whose second word is the ticket of
// the last completed command and whose fourth is its result; the title
// checks it one block later and treats a late command as an overrun.
// Commands complete when submitted here.
//
// The transforms themselves are not computed: the hardware takes impulse
// responses already in its own partitioned frequency-domain layout, which
// is not known. Outputs are silence, so reverb returns are dry.
// shortcut: no convolution or block transform; outputs are zeros. Needs the
// coprocessor's impulse response and spectrum layouts to do better.
// Debug log lines are not ported (a release build logs info and above).
#include <cstring>
#include <mutex>

#include "core/log.hpp"
#include "modules.hpp"

namespace danzi::hle::dsp {

namespace {

constexpr HRESULT_ E_POINTER_ = static_cast<HRESULT_>(0x80004003u);
constexpr HRESULT_ E_OUTOFMEMORY_ = static_cast<HRESULT_>(0x8007000Eu);

constexpr std::uint32_t CONVOLUTION = 1;

constexpr std::uint32_t STEREO_OPTIONS = 2 | 8;
constexpr std::uint32_t NOT_FOR_CONVOLUTION = 4;

}   

struct Dsp {
    std::uint32_t max_streams;
    std::mutex lock;
    std::uint32_t active = 0;
};


#pragma pack(push, 1)
struct StreamDesc {
    std::uint32_t kind;
    std::uint32_t options;
    std::uint32_t frames;
    std::uint32_t channels;
    std::uint32_t ir_floats;
    const float* ir;
};
#pragma pack(pop)
static_assert(sizeof(StreamDesc) == 28);


struct Status {
    std::uint32_t submitted;
    std::uint32_t completed;
    std::uint32_t reserved;
    HRESULT_ result;
};

struct Stream {
    Dsp* dsp;
    std::uint32_t frames;
    std::uint32_t channels;
    Status* status;
};


struct Command {
    float gains[2];
    const float* input;
    float* output;
    std::uint64_t reserved;
};
static_assert(sizeof(Command) == 32);

HRESULT_ XDspConnectWithMaximumStreamLimit(void* base, std::uint32_t base_length, std::uint32_t ir_seconds, std::uint32_t max_streams, std::uint32_t max_channels,
                                           Dsp** out) {
    if (!base || base_length < 0x1'0000 || base_length > 0x1F40'0000 || ir_seconds > 0x80 || max_streams > 0x100 || max_channels > 2) {
        return E_INVALIDARG_;
    }
    if (!out) {
        return E_POINTER_;
    }
    log::info("victory_hle::modules::dsp", "audio DSP connected",
              {{"base_length", log::debug(base_length)}, {"ir_seconds", log::debug(ir_seconds)}, {"max_streams", log::debug(max_streams)},
               {"max_channels", log::debug(max_channels)}});
    *out = new Dsp{max_streams};
    return S_OK_;
}

HRESULT_ XDspDisconnect(Dsp* dsp) {
    if (!dsp) {
        return E_INVALIDARG_;
    }


    delete dsp;
    return S_OK_;
}

namespace {

bool valid(const StreamDesc& desc) {
    std::uint32_t kind = desc.kind, options = desc.options, frames = desc.frames, channels = desc.channels;
    bool shape = kind == CONVOLUTION ? channels >= 1 && channels <= 2 && (frames == 0x200 || frames == 0x400) && (options & NOT_FOR_CONVOLUTION) == 0
                                     : channels == 1 && (frames == 0x200 || frames == 0x400 || frames == 0x800);
    return shape && ((options & STEREO_OPTIONS) == 0 || channels == 2);
}

}   

HRESULT_ XDspActivate(Dsp* dsp, const StreamDesc* desc, const Status** status, Stream** stream) {
    if (!status || !stream) {
        return E_POINTER_;
    }
    if (!dsp || !desc) {
        return E_INVALIDARG_;
    }
    if (!valid(*desc)) {
        return E_INVALIDARG_;
    }
    {
        std::lock_guard l(dsp->lock);
        if (dsp->active >= dsp->max_streams) {
            return E_OUTOFMEMORY_;
        }
        dsp->active += 1;
    }
    auto* made = new Stream{dsp, desc->frames, desc->channels, new Status{0, 0, 0, S_OK_}};
    *status = made->status;
    *stream = made;
    return S_OK_;
}

HRESULT_ XDspDeactivate(Stream* stream) {
    if (!stream) {
        return E_INVALIDARG_;
    }


    Dsp* dsp = stream->dsp;
    {
        std::lock_guard l(dsp->lock);
        dsp->active -= 1;
    }
    delete stream->status;
    delete stream;
    return S_OK_;
}


HRESULT_ XDspSubmitCommand(Stream* stream, const Command* command, std::uint32_t* ticket) {
    if (!stream || !command) {
        return E_INVALIDARG_;
    }
    if (command->output) {
        std::size_t samples = stream->frames * stream->channels;

        std::memset(command->output, 0, samples * sizeof(float));
    }

    auto* status = static_cast<volatile Status*>(stream->status);
    std::uint32_t next = status->submitted + 1;
    status->submitted = next;
    status->result = S_OK_;
    status->completed = next;
    if (ticket) {
        *ticket = next;
    }
    return S_OK_;
}

#define E(f) HleExport{#f, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() {
    return {E(XDspConnectWithMaximumStreamLimit), E(XDspDisconnect), E(XDspActivate), E(XDspDeactivate), E(XDspSubmitCommand)};
}
#undef E

}   
