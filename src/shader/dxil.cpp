// SPDX-License-Identifier: GPL-3.0-or-later
// A DXIL shader under construction (`emit/shader.rs`; DanziLoader /
// victory-perf, see NOTICE.md). See dxil.hpp.
#include "dxil.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

namespace danzi::shader::dxil {

namespace {
std::atomic<std::uint32_t> g_view_limit{0};
}

void set_view_heap_limit(std::uint32_t null_slot) { g_view_limit.store(null_slot, std::memory_order_relaxed); }
std::uint32_t view_heap_limit() { return g_view_limit.load(std::memory_order_relaxed); }

std::uint32_t parse_shader_opt(const char* value) {
    if (!value) {
        return bitcode::opt::ALL;
    }
    std::string v(value);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\n' || v.front() == '\r')) {
        v.erase(v.begin());
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\n' || v.back() == '\r')) {
        v.pop_back();
    }
    std::uint64_t n = 0;
    std::string_view digits = v;
    int base = 10;
    if (digits.starts_with("0x")) {
        digits.remove_prefix(2);
        base = 16;
    }
    if (digits.empty()) {
        return bitcode::opt::ALL;
    }
    for (char c : digits) {
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return bitcode::opt::ALL;
        }
        n = n * base + d;
        if (n > 0xFFFFFFFFull) {
            return bitcode::opt::ALL;
        }
    }
    return static_cast<std::uint32_t>(n) & bitcode::opt::ALL;
}

std::uint32_t shader_opt() {
    static const std::uint32_t passes = [] {
        char* v = nullptr;
        std::size_t n = 0;
        if (_dupenv_s(&v, &n, "VICTORY_SHADER_OPT") != 0 || !v) {
            return parse_shader_opt(nullptr);
        }
        std::uint32_t p = parse_shader_opt(v);
        std::free(v);
        return p;
    }();
    return passes;
}

Shader::Shader(bool px) : pixel(px) {
    Ty void_t = m.void_();
    Ty i1 = m.int_(1);
    Ty i8 = m.int_(8);
    Ty i32 = m.int_(32);
    Ty f32 = m.float_();
    Ty i8p = m.ptr(i8);
    Ty handle = m.named_struct("dx.types.Handle", {i8p});
    Ty props = m.named_struct("dx.types.ResourceProperties", {i32, i32});
    Ty bind = m.named_struct("dx.types.ResBind", {i32, i32, i32, i8});
    Ty cbuf_ret = m.named_struct("dx.types.CBufRet.i32", {i32, i32, i32, i32});
    Ty res_ret = m.named_struct("dx.types.ResRet.f32", {f32, f32, f32, f32, i32});
    Ty main_ty = m.func_ty(void_t, {});
    m.ptr(main_ty);
    main_ = m.function("main", main_ty, std::nullopt, true);
    entry = m.new_block();
    t = Types{void_t, i1, i32, f32, handle, props, bind, cbuf_ret, res_ret};
}

V Shader::lds() {
    if (lds_) {
        return *lds_;
    }
    Ty arr = m.array(lds_dwords, t.i32);
    V g = m.global("lds", arr, 3);
    lds_ = g;
    return g;
}

V Shader::op(std::size_t block, std::string_view name, Attr attr, Ty ret, std::uint32_t opcode, const std::vector<V>& args) {
    std::vector<Ty> params;
    params.reserve(args.size() + 1);
    params.push_back(t.i32);
    for (V a : args) {
        params.push_back(m.type_of(a));
    }
    Ty fty = m.func_ty(ret, std::move(params));
    m.ptr(fty);
    name_buf_.assign("dx.op.");
    name_buf_.append(name);
    V f = m.function(name_buf_, fty, attr, false);
    std::vector<V> all;
    all.reserve(args.size() + 1);
    all.push_back(m.i32(opcode));
    all.insert(all.end(), args.begin(), args.end());
    return m.call(block, f, std::move(all));
}

V Shader::user_data_handle() {
    V b0 = m.i32(0);
    V b1 = m.i32(0);
    V b2 = m.i32(USER_DATA_SPACE);
    V b3 = m.i8(2);
    V bind = m.aggregate(t.bind, {b0, b1, b2, b3});
    std::size_t block = entry;
    V i = m.i32(0);
    V f = m.boolean(false);
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, i, f});
    return annotate(block, h, 13, UINT32_MAX);
}

V Shader::root_cbv_handle(std::uint8_t slot) {
    for (const auto& [s, h] : root_cbvs) {
        if (s == slot) {
            return h;
        }
    }
    V r = m.i32(slot);
    V space = m.i32(ROOT_CBV_SPACE);
    V kind = m.i8(2);
    V bind = m.aggregate(t.bind, {r, r, space, kind});
    V f = m.boolean(false);
    std::size_t block = entry;
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, r, f});
    h = annotate(block, h, 13, 65536);
    root_cbvs.emplace_back(slot, h);
    return h;
}

V Shader::root_srv_handle(std::uint8_t slot) {
    for (const auto& [s, h] : root_srvs) {
        if (s == slot) {
            return h;
        }
    }
    V r = m.i32(slot);
    V space = m.i32(ROOT_CBV_SPACE);
    V kind = m.i8(0);
    V bind = m.aggregate(t.bind, {r, r, space, kind});
    V f = m.boolean(false);
    std::size_t block = entry;
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, r, f});
    h = annotate(block, h, 11, 0);
    flags |= flags::RAW_BUFFERS;
    root_srvs.emplace_back(slot, h);
    return h;
}

V Shader::root_uav_handle(std::uint8_t slot) {
    for (const auto& [s, h] : root_uavs) {
        if (s == slot) {
            return h;
        }
    }
    V r = m.i32(slot);
    V space = m.i32(ROOT_CBV_SPACE);
    V kind = m.i8(1);
    V bind = m.aggregate(t.bind, {r, r, space, kind});
    V f = m.boolean(false);
    std::size_t block = entry;
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, r, f});
    h = annotate(block, h, 11 | 0x1000, 0);
    flags |= flags::RAW_BUFFERS;
    root_uavs.emplace_back(slot, h);
    return h;
}

V Shader::sampler_handle(std::size_t block, std::uint32_t n) {
    V b0 = m.i32(n);
    V b1 = m.i32(n);
    V b2 = m.i32(SAMPLER_SPACE);
    V b3 = m.i8(3);
    V bind = m.aggregate(t.bind, {b0, b1, b2, b3});
    V i = m.i32(n);
    V f = m.boolean(false);
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, i, f});
    std::uint32_t cmp = (cmp_samplers >> n & 1) ? 0x8000 : 0;
    return annotate(block, h, 14 | cmp, 0);
}

V Shader::annotate(std::size_t block, V h, std::uint32_t props0, std::uint32_t props1) {
    V p0 = m.i32(props0);
    V p1 = m.i32(props1);
    V p = m.aggregate(t.props, {p0, p1});
    return op(block, "annotateHandle", Attr::ReadNone, t.handle, 216, {h, p});
}

V Shader::heap_handle(std::size_t block, V index, bool sampler, bool nonuniform, std::pair<std::uint32_t, std::uint32_t> props) {
    std::uint32_t limit = view_heap_limit();
    if (!sampler && limit != 0) {
        V null = m.i32(limit);
        V v = op(block, "binary.i32", Attr::ReadNone, t.i32, 40, {index, null});
        m.tag(v, bitcode::Tag::Clamp);
        index = v;
    } else if (sampler) {
        V last = m.i32(2047);
        V v = op(block, "binary.i32", Attr::ReadNone, t.i32, 40, {index, last});
        m.tag(v, bitcode::Tag::Clamp);
        index = v;
    }
    flags |= sampler ? flags::SAMPLER_HEAP : flags::RESOURCE_HEAP;
    nonuniform_handles += nonuniform ? 1 : 0;
    V s = m.boolean(sampler);
    V n = m.boolean(nonuniform);
    V h = op(block, "createHandleFromHeap", Attr::ReadNone, t.handle, 218, {index, s, n});
    return annotate(block, h, props.first, props.second);
}

Md Shader::i32c(std::uint32_t v) {
    V c = m.i32(v);
    return m.md_value(c);
}

void Shader::fix_user_data_size(std::uint32_t size) {
    V o0 = m.i32(13);
    V o1 = m.i32(UINT32_MAX);
    V old = m.aggregate(t.props, {o0, o1});
    V n0 = m.i32(13);
    V n1 = m.i32(size);
    V now = m.aggregate(t.props, {n0, n1});
    m.replace_const(old, now);
}

std::optional<Md> Shader::signature_md(const std::vector<SigElement>& elements) {
    if (elements.empty()) {
        return std::nullopt;
    }
    std::vector<std::optional<Md>> list;
    auto c32 = [&](std::uint32_t v) -> std::optional<Md> {
        V c = m.i32(v);
        return m.md_value(c);
    };
    auto c8 = [&](std::uint8_t v) -> std::optional<Md> {
        V c = m.i8(v);
        return m.md_value(c);
    };
    for (std::size_t id = 0; id < elements.size(); ++id) {
        const SigElement& e = elements[id];
        auto idx = c32(e.index);
        std::optional<Md> indices = m.md_node({idx});
        std::optional<Md> usage;
        if (e.usage != 0) {
            auto tag = c32(3);
            auto mask = c32(e.usage);
            usage = m.md_node({tag, mask});
        }
        std::vector<std::optional<Md>> ops;
        ops.push_back(c32(static_cast<std::uint32_t>(id)));
        ops.push_back(m.md_str(e.name));
        ops.push_back(c8(e.comp));
        ops.push_back(c8(e.kind));
        ops.push_back(indices);
        ops.push_back(c8(e.interp));
        ops.push_back(c32(1));
        ops.push_back(c8(e.cols));
        ops.push_back(c32(e.row.value_or(UINT32_MAX)));
        ops.push_back(c8(e.row ? e.col : std::uint8_t{0xFF}));
        ops.push_back(usage);
        list.push_back(m.md_node(std::move(ops)));
    }
    return m.md_node(std::move(list));
}

std::vector<std::uint8_t> Shader::finish() {
    std::uint32_t ud_size = (user_data * 4 + 15) / 16 * 16;
    fix_user_data_size(ud_size);
    std::uint32_t passes = shader_opt() & bitcode::opt::MODULE;
    if (passes != 0) {
        m.optimise(passes);
        bitcode::Usage used = m.usage();
        std::pair<std::uint64_t, bool> checks[] = {
            {flags::WAVE_OPS, used.wave_ops},
            {flags::INT64, used.int64},
            {flags::TYPED_UAV_LOADS, used.typed_uav_loads},
            {flags::RESOURCE_HEAP, used.heap_views},
            {flags::SAMPLER_HEAP, used.heap_samplers},
            {flags::ATOMIC_TYPED64 | flags::ATOMIC_HEAP64, used.atomic64},
        };
        for (const auto& [flag, u] : checks) {
            if (!u) {
                flags &= ~flag;
            }
        }
    }
    std::optional<Md> resources;
    std::vector<container::Binding> bindings;
    if (user_data > 0 || samplers > 0 || !root_cbvs.empty() || !root_srvs.empty() || !root_uavs.empty()) {
        Md empty = m.md_str("");
        std::optional<Md> cbvs;
        std::vector<std::optional<Md>> cbv_list;
        if (user_data > 0) {
            Ty arr = m.array((user_data + 3) / 4 * 4, t.i32);
            Ty ty = m.named_struct("UserData", {arr});
            Ty p = m.ptr(ty);
            V u = m.undef(p);
            std::vector<std::optional<Md>> ops;
            ops.push_back(i32c(0));
            ops.push_back(m.md_value(u));
            ops.push_back(empty);
            ops.push_back(i32c(USER_DATA_SPACE));
            ops.push_back(i32c(0));
            ops.push_back(i32c(1));
            ops.push_back(i32c(ud_size));
            ops.push_back(std::nullopt);
            cbv_list.push_back(m.md_node(std::move(ops)));
            bindings.push_back({2, USER_DATA_SPACE, 0, 13});
        }
        Ty blob = m.array(16384, t.i32);
        Ty ty = m.named_struct("RootCbv", {blob});
        Ty p = m.ptr(ty);
        V u = m.undef(p);
        for (const auto& [slot, _] : std::vector(root_cbvs)) {
            std::vector<std::optional<Md>> ops;
            ops.push_back(i32c(static_cast<std::uint32_t>(cbv_list.size())));
            ops.push_back(m.md_value(u));
            ops.push_back(empty);
            ops.push_back(i32c(ROOT_CBV_SPACE));
            ops.push_back(i32c(slot));
            ops.push_back(i32c(1));
            ops.push_back(i32c(65536));
            ops.push_back(std::nullopt);
            cbv_list.push_back(m.md_node(std::move(ops)));
            bindings.push_back({2, ROOT_CBV_SPACE, slot, 13});
        }
        if (!cbv_list.empty()) {
            cbvs = m.md_node(std::move(cbv_list));
        }
        std::optional<Md> srvs;
        if (!root_srvs.empty()) {
            Ty sty = m.named_struct("struct.ByteAddressBuffer", {t.i32});
            Ty sp = m.ptr(sty);
            V su = m.undef(sp);
            std::vector<std::optional<Md>> list;
            std::uint32_t n = 0;
            for (const auto& [slot, _] : std::vector(root_srvs)) {
                std::vector<std::optional<Md>> ops;
                ops.push_back(i32c(n++));
                ops.push_back(m.md_value(su));
                ops.push_back(empty);
                ops.push_back(i32c(ROOT_CBV_SPACE));
                ops.push_back(i32c(slot));
                ops.push_back(i32c(1));
                ops.push_back(i32c(11));
                ops.push_back(i32c(0));
                ops.push_back(std::nullopt);
                list.push_back(m.md_node(std::move(ops)));
                bindings.push_back({4, ROOT_CBV_SPACE, slot, 11});
            }
            srvs = m.md_node(std::move(list));
        }
        std::optional<Md> uavs;
        if (!root_uavs.empty()) {
            Ty uty = m.named_struct("struct.RWByteAddressBuffer", {t.i32});
            Ty up = m.ptr(uty);
            V uu = m.undef(up);
            V no_c = m.boolean(false);
            Md no = m.md_value(no_c);
            std::vector<std::optional<Md>> list;
            std::uint32_t n = 0;
            for (const auto& [slot, _] : std::vector(root_uavs)) {
                std::vector<std::optional<Md>> ops;
                ops.push_back(i32c(n++));
                ops.push_back(m.md_value(uu));
                ops.push_back(empty);
                ops.push_back(i32c(ROOT_CBV_SPACE));
                ops.push_back(i32c(slot));
                ops.push_back(i32c(1));
                ops.push_back(i32c(11));
                ops.push_back(no);
                ops.push_back(no);
                ops.push_back(no);
                ops.push_back(std::nullopt);
                list.push_back(m.md_node(std::move(ops)));
                bindings.push_back({7, ROOT_CBV_SPACE, slot, 11});
            }
            uavs = m.md_node(std::move(list));
        }
        std::optional<Md> smps;
        if (samplers > 0) {
            Ty sty = m.named_struct("struct.SamplerState", {t.i32});
            Ty sp = m.ptr(sty);
            V su = m.undef(sp);
            std::vector<std::optional<Md>> list;
            for (std::uint32_t n = 0; n < samplers; ++n) {
                auto kind = static_cast<std::uint32_t>(cmp_samplers >> n & 1);
                std::vector<std::optional<Md>> ops;
                ops.push_back(i32c(n));
                ops.push_back(m.md_value(su));
                ops.push_back(empty);
                ops.push_back(i32c(SAMPLER_SPACE));
                ops.push_back(i32c(n));
                ops.push_back(i32c(1));
                ops.push_back(i32c(kind));
                ops.push_back(std::nullopt);
                list.push_back(m.md_node(std::move(ops)));
                bindings.push_back({1, SAMPLER_SPACE, n, 14});
            }
            smps = m.md_node(std::move(list));
        }
        resources = m.md_node({srvs, uavs, cbvs, smps});

        auto rank = [](const container::Binding& b) {
            switch (b.kind) {
                case 2: return 0;
                case 1: return 1;
                case 7: return 3;
                default: return 2;
            }
        };
        std::stable_sort(bindings.begin(), bindings.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    }
    if (resources) {
        m.named_md("dx.resources", {*resources});
    }

    bool graphics = !threads || mesh;
    if (graphics) {
        auto scalars = [](const std::vector<SigElement>& els) {
            std::uint32_t mx = 0;
            for (const auto& e : els) {
                if (e.row) {
                    mx = std::max(mx, *e.row * 4 + e.col + e.cols);
                }
            }
            return mx;
        };
        std::uint32_t ni = scalars(inputs), no = scalars(outputs);
        std::vector<std::uint32_t> view{ni, no};
        if (mesh) {
            view.push_back(scalars(prims));
        }
        for (std::uint32_t i = 0; i < ni; ++i) {
            for (std::uint32_t k = 0; k < (no + 31) / 32; ++k) {
                std::uint32_t bits = std::min<std::uint32_t>(no - 32 * k, 32);
                view.push_back(bits == 32 ? UINT32_MAX : (1u << bits) - 1);
            }
        }
        if (geometry) {
            view.insert(view.end(), {0, 0, 0});
        }
        Ty arr = m.array(static_cast<std::uint32_t>(view.size()), t.i32);
        std::vector<V> elems;
        elems.reserve(view.size());
        for (std::uint32_t v : view) {
            elems.push_back(m.i32(v));
        }
        V agg = m.aggregate(arr, elems);
        Md amd = m.md_value(agg);
        Md view_node = m.md_node({amd});
        m.named_md("dx.viewIdState", {view_node});
    }

    std::optional<Md> sigs;
    if (graphics) {
        auto ins = signature_md(inputs);
        auto outs = signature_md(outputs);
        auto ps = signature_md(prims);
        sigs = m.md_node({ins, outs, ps});
    }
    if (pixel && std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind == 20; })) {
        flags |= flags::STENCIL_REF;
    }
    std::vector<Md> props;
    if (flags != 0) {
        props.push_back(i32c(0));
        Ty i64t = m.int_(64);
        V f = m.int_const(i64t, static_cast<std::int64_t>(flags));
        props.push_back(m.md_value(f));
    }
    if (geometry) {
        props.push_back(i32c(1));
        std::vector<std::optional<Md>> state;
        for (std::uint32_t n : {3u, geometry->max_vertices, 1u, 5u, 1u}) {
            state.push_back(i32c(n));
        }
        props.push_back(m.md_node(std::move(state)));
    }
    if (threads) {
        std::vector<std::optional<Md>> tn;
        for (std::uint32_t n : *threads) {
            tn.push_back(i32c(n));
        }
        Md tnode = m.md_node(std::move(tn));
        if (mesh) {
            props.push_back(i32c(9));
            std::vector<std::optional<Md>> node{tnode};
            for (std::uint32_t n : {mesh->max_vertices, mesh->max_primitives, std::uint32_t{mesh->topology}, 0u}) {
                node.push_back(i32c(n));
            }
            props.push_back(m.md_node(std::move(node)));
        } else {
            props.push_back(i32c(4));
            props.push_back(tnode);
        }
    }
    if (wave_size) {
        props.push_back(i32c(11));
        Md n = i32c(*wave_size);
        props.push_back(m.md_node({n}));
    }
    std::optional<Md> props_node;
    if (!props.empty()) {
        std::vector<std::optional<Md>> ops(props.begin(), props.end());
        props_node = m.md_node(std::move(ops));
    }
    Md main_md = m.md_value(main_);
    Md name = m.md_str("main");
    Md entry_md = m.md_node({main_md, name, sigs, resources, props_node});

    Md ident = m.md_str("victory");
    Md ident_node = m.md_node({ident});
    m.named_md("llvm.ident", {ident_node});
    Md one = i32c(1);
    Md six = i32c(6);
    Md version = m.md_node({one, six});
    m.named_md("dx.version", {version});
    m.named_md("dx.valver", {version});
    const char* sm_name;
    std::uint32_t kind;
    if (mesh) {
        sm_name = "ms";
        kind = 13;
    } else if (geometry) {
        sm_name = "gs";
        kind = 2;
    } else if (threads) {
        sm_name = "cs";
        kind = 5;
    } else if (pixel) {
        sm_name = "ps";
        kind = 0;
    } else {
        sm_name = "vs";
        kind = 1;
    }
    Md smn = m.md_str(sm_name);
    Md sm = m.md_node({smn, six, six});
    m.named_md("dx.shaderModel", {sm});
    m.named_md("dx.entryPoints", {entry_md});

    std::vector<std::uint8_t> code = m.write();
    std::vector<std::uint8_t> dxil;
    dxil.reserve(24 + code.size());
    auto put = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            dxil.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    put(kind << 16 | 0x66);
    put(static_cast<std::uint32_t>((24 + code.size()) / 4));
    dxil.insert(dxil.end(), {'D', 'X', 'I', 'L'});
    put(0x106);
    put(16);
    put(static_cast<std::uint32_t>(code.size()));
    dxil.insert(dxil.end(), code.begin(), code.end());

    std::uint64_t features = 0;
    if (flags & flags::DOUBLES) {
        features |= 0x1;
    }
    if (flags & flags::STENCIL_REF) {
        features |= 0x200;
    }
    if (flags & flags::TYPED_UAV_LOADS) {
        features |= 0x800;
    }
    if (flags & flags::WAVE_OPS) {
        features |= 0x4000;
    }
    if (flags & flags::INT64) {
        features |= 0x8000;
    }
    if (flags & flags::BARYCENTRICS) {
        features |= 0x20000;
    }
    if (flags & flags::RESOURCE_HEAP) {
        features |= 0x2000000;
    }
    if (flags & flags::SAMPLER_HEAP) {
        features |= 0x4000000;
    }
    if (flags & flags::ATOMIC_TYPED64) {
        features |= 0x400000;
    }
    if (flags & flags::ATOMIC_HEAP64) {
        features |= 0x10000000;
    }
    std::uint8_t sfi0[8];
    for (int k = 0; k < 8; ++k) {
        sfi0[k] = static_cast<std::uint8_t>(features >> (8 * k));
    }
    auto isg1 = container::signature(inputs, true);
    auto osg1 = container::signature(outputs, false);
    bool flag = pixel ? std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind >= 17 && o.kind <= 19; })
                      : std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind == 3; });
    container::Psv pv;
    pv.stage = static_cast<std::uint8_t>(kind);
    pv.flag = flag && !threads;
    pv.inputs = &inputs;
    pv.outputs = &outputs;
    pv.prims = &prims;
    if (mesh) {
        pv.mesh = std::pair{*mesh, lds_dwords * 4};
    }
    if (geometry) {
        pv.geometry = geometry->max_vertices;
    }
    pv.bindings = &bindings;
    pv.threads = threads ? *threads : std::array<std::uint32_t, 3>{};
    pv.wave_size = wave_size;
    auto psv0 = container::psv(pv);
    std::uint8_t hash[20] = {};
    auto psg1 = container::signature(prims, false);
    std::vector<std::pair<const char*, std::span<const std::uint8_t>>> parts{{"SFI0", sfi0}, {"ISG1", isg1}, {"OSG1", osg1}};
    if (mesh) {
        parts.emplace_back("PSG1", psg1);
    }
    parts.emplace_back("PSV0", psv0);
    parts.emplace_back("HASH", hash);
    parts.emplace_back("DXIL", dxil);
    return container::build(parts);
}

}   
