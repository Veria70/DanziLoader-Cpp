// SPDX-License-Identifier: GPL-3.0-or-later
// Debugging aid (`victory-hle/src/modules/audio/capture.rs`, the
// reference's `diagnostics` feature; DanziLoader / victory-perf, see
// NOTICE.md): `VICTORY_AUDIO_CAPTURE=<file.wav>` records what the title
// plays, 48 kHz stereo 16-bit, through process loopback: our own process
// tree only, never the rest of the host's mix. Stretches where nothing was
// played are written as silence, so the file keeps real time.
//
// The recorder runs in a child copy of this executable: an audio client in
// the title's process raises that process's timer resolution, which changes
// how the title's own waits behave.
//
// The reference compiles this file only with its `diagnostics` feature, so
// its log lines are not ported, nor the timing probe `start` runs when this
// module's debug log is on (once a second, what a `Sleep(1)` takes).
// Nothing calls these until the launcher (milestone 7) does.
#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "modules.hpp"

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

namespace danzi::hle::audio::capture {

namespace {

constexpr std::uint32_t RATE = 48'000;
constexpr std::size_t FRAME_BYTES = 4;


constexpr wchar_t TARGET[] = L"VICTORY_AUDIO_CAPTURE_OF";

std::optional<std::wstring> env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? std::nullopt : std::optional<std::wstring>(L"");
    }
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}


class Activated final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (iid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = InterlockedDecrement(&refs_);
        if (left == 0) {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { return S_OK; }

private:
    LONG refs_ = 1;
};


template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) {
            p->Release();
        }
    }
    T* operator->() const { return p; }
};

std::array<std::uint8_t, 44> header(std::uint64_t frames) {
    std::uint64_t bytes = frames * FRAME_BYTES;
    std::uint32_t data = bytes <= UINT32_MAX ? static_cast<std::uint32_t>(bytes) : UINT32_MAX - 36;
    std::array<std::uint8_t, 44> h{};
    auto put = [&](std::size_t at, const void* v, std::size_t n) { std::memcpy(h.data() + at, v, n); };
    auto u32 = [&](std::size_t at, std::uint32_t v) { put(at, &v, 4); };
    auto u16 = [&](std::size_t at, std::uint16_t v) { put(at, &v, 2); };
    put(0, "RIFF", 4);
    u32(4, 36 + data);
    put(8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, 2);
    u32(24, RATE);
    u32(28, RATE * FRAME_BYTES);
    u16(32, FRAME_BYTES);
    u16(34, 16);
    put(36, "data", 4);
    u32(40, data);
    return h;
}

bool write_all(HANDLE file, const void* data, std::size_t len) {
    auto* p = static_cast<const std::uint8_t*>(data);
    while (len > 0) {
        DWORD wrote = 0;
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(len, 0x4000'0000));
        if (!WriteFile(file, p, chunk, &wrote, nullptr) || wrote == 0) {
            return false;
        }
        p += wrote;
        len -= wrote;
    }
    return true;
}


bool record(std::uint32_t pid, const std::wstring& path) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return false;
    }
    HANDLE target = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!target) {
        return false;
    }
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT blob{};
    blob.vt = VT_BLOB;
    blob.blob.cbSize = sizeof params;
    blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    auto* handler = new Activated;
    Com<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &blob, handler, &operation.p);
    handler->Release();
    if (FAILED(hr)) {
        return false;
    }
    Com<IAudioClient> client;
    for (;;) {
        HRESULT result = S_OK;
        Com<IUnknown> unknown;

        if (SUCCEEDED(operation->GetActivateResult(&result, &unknown.p))) {
            if (FAILED(result) || !unknown.p || FAILED(unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client.p)))) {
                return false;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = RATE;
    format.nAvgBytesPerSec = RATE * FRAME_BYTES;
    format.nBlockAlign = FRAME_BYTES;
    format.wBitsPerSample = 16;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 10'000'000, 0, &format, nullptr))) {
        return false;
    }
    Com<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture.p)))) {
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{file};
    auto h0 = header(0);
    if (!write_all(file, h0.data(), h0.size()) || FAILED(client->Start())) {
        return false;
    }

    std::uint64_t written = 0;
    std::optional<std::uint64_t> first;
    auto patched = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bool ended = WaitForSingleObject(target, 0) == WAIT_OBJECT_0;
        for (;;) {
            UINT32 next = 0;
            if (FAILED(capture->GetNextPacketSize(&next))) {
                return false;
            }
            if (next == 0) {
                break;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) {
                return false;
            }

            if (!first) {
                first = qpc;
            }
            std::uint64_t at = (qpc - *first) * RATE / 10'000'000;
            if (at > written + RATE / 500) {
                std::vector<std::uint8_t> silence(static_cast<std::size_t>(at - written) * FRAME_BYTES, 0);
                if (!write_all(file, silence.data(), silence.size())) {
                    return false;
                }
                written = at;
            }
            std::size_t bytes = std::size_t{frames} * FRAME_BYTES;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                std::vector<std::uint8_t> zeros(bytes, 0);
                if (!write_all(file, zeros.data(), bytes)) {
                    return false;
                }
            } else if (!write_all(file, data, bytes)) {
                return false;
            }
            written += frames;
            if (FAILED(capture->ReleaseBuffer(frames))) {
                return false;
            }
        }
        if (ended || std::chrono::steady_clock::now() - patched >= std::chrono::seconds(1)) {

            auto h = header(written);
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !write_all(file, h.data(), h.size()) || !SetFilePointerEx(file, zero, nullptr, FILE_END)) {
                return false;
            }
            patched = std::chrono::steady_clock::now();
        }
        if (ended) {
            return true;
        }
    }
}


std::optional<std::uint32_t> parse_u32(const std::wstring& s) {
    std::size_t i = !s.empty() && s[0] == L'+' ? 1 : 0;
    if (i == s.size()) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') {
            return std::nullopt;
        }
        v = v * 10 + (s[i] - L'0');
        if (v > UINT32_MAX) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(v);
}

}   


void start() {
    if (!env(L"VICTORY_AUDIO_CAPTURE")) {
        return;
    }
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (n == 0) {
            return;
        }
        if (n < exe.size()) {
            exe.resize(n);
            break;
        }
        exe.resize(exe.size() * 2);
    }
    auto upper = [](std::wstring s) {
        for (auto& c : s) {
            c = static_cast<wchar_t>(std::towupper(c));
        }
        return s;
    };
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);

            std::size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) {
                continue;
            }
            std::wstring name = entry.substr(0, eq);
            vars[upper(name)] = {name, entry.substr(eq + 1)};
        }
        FreeEnvironmentStringsW(block);
    }
    vars[upper(TARGET)] = {TARGET, std::to_wstring(GetCurrentProcessId())};
    std::wstring env_block;
    for (auto& [key, nv] : vars) {
        env_block += nv.first + L"=" + nv.second;
        env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, env_block.data(), nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}


bool child() {
    auto pid = env(TARGET);
    auto path = env(L"VICTORY_AUDIO_CAPTURE");
    std::optional<std::uint32_t> id = pid ? parse_u32(*pid) : std::nullopt;
    if (!id || !path) {
        return false;
    }
    record(*id, *path);
    return true;
}

}   
