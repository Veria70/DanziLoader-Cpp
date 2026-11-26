// SPDX-License-Identifier: GPL-3.0-or-later
// Host D3D12 checks for emitted shaders (`crates/victory-shader/src/emit/
// host.rs`, its non-test part; DanziLoader / victory-perf, see NOTICE.md):
// pipeline creation for the offline batch (`translate --pipelines`), with
// the root signature translated shaders expect and stub stages. Errors are
// the reference's texts (`windows::core::Error::message`).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <optional>
#include <string>

#include "emit.hpp"

namespace danzi::shader::emit::host {

struct Host {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
};


std::optional<Host> make_host(bool warp, std::string& error);


std::optional<std::string> compute_pipeline(const Host& h, const HostShader& cs);

std::optional<std::string> pipeline(const Host& h, const HostShader& vs, const HostShader& ps);

std::optional<std::string> mesh_pipeline(const Host& h, const HostShader& ms, const HostShader& ps);


std::optional<HostShader> stub_vs(const HostShader& ps, std::string& error);

HostShader stub_ps();

}   
