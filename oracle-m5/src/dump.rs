// SPDX-License-Identifier: GPL-3.0-or-later

use std::fmt::Write as _;
use std::path::Path;

use victory_shader::{Container, FourCC};

  fn native(data: &[u8], out: &mut String) {
    let c = match Container::parse(data) {
        Ok(c) => c,
        Err(e) => {
            let _ = writeln!(out, " container err {e}");
            return;
        }
    };
    for p in &c.parts {
        let _ = writeln!(out, " part {} {}", p.fourcc, p.data.len());
    }
    let Some(dxil) = c.part(FourCC::DXIL) else {
        return;
    };
    match victory_shader::native_programs(dxil.data) {
        Err(e) => {
            let _ = writeln!(out, " native err {e}");
        }
        Ok(programs) => {
            for p in programs {
                let _ = writeln!(
                    out,
                    " program {:?} wave64 {} vgprs {} code {} {}",
                    p.stage,
                    p.wave64,
                    p.vgprs,
                    p.code.len(),
                    &blake3::hash(&p.code).to_hex()[..16]
                );
                for l in &p.layout {
                    let _ = writeln!(
                        out,
                        "  layout {:?} {} {} {} {} {}",
                        l.root, l.table_offset_dwords, l.sgprs, l.base_register, l.user_data, l.slot
                    );
                }
                for u in &p.used {
                    let _ = writeln!(
                        out,
                        "  used {:?} {:?} {} {} {} {} {} {}",
                        u.kind, u.root, u.table_offset_dwords, u.lower_bound, u.stride, u.dwords, u.user_data, u.slot
                    );
                }
            }
        }
    }
}

  pub fn picked(data: &[u8], out: &mut String) -> Option<victory_shader::NativeProgram> {
    use victory_shader::HwStage;
    let c = match Container::parse(data) {
        Ok(c) => c,
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            return None;
        }
    };
    let Some(dxil) = c.part(FourCC::DXIL) else {
        let _ = writeln!(out, " err no DXIL part");
        return None;
    };
    match victory_shader::native_programs(dxil.data) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            None
        }
        Ok(programs) => {
            let p = programs.into_iter().find(|p| matches!(p.stage, HwStage::Vs | HwStage::Ps | HwStage::Cs | HwStage::Ms));
            if p.is_none() {
                let _ = writeln!(out, " err no program");
            }
            p
        }
    }
}

 fn ir(data: &[u8], out: &mut String) {
    let Some(p) = picked(data, out) else { return };
    match victory_shader::ir::build(&p.patched_code(), p.wave64, p.vgprs) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
        }
        Ok(program) => crate::irdump::program(&program, out),
    }
}

     fn spill(data: &[u8], out: &mut String) {
    let Some(p) = picked(data, out) else { return };
    match victory_shader::spill::build(&p) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
        }
        Ok(program) => crate::irdump::program(&program, out),
    }
}

 fn structure(data: &[u8], out: &mut String) {
    let Some(p) = picked(data, out) else { return };
    let program = match victory_shader::spill::build(&p) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            return;
        }
        Ok(program) => program,
    };
    match victory_shader::structure::structure(&program) {
        Err(e) => {
            let _ = writeln!(out, " st err {e}");
        }
        Ok(st) => crate::stdump::structured(&st, out),
    }
}

fn addr(a: &victory_shader::resource::Address) -> String {
    use victory_shader::resource::{CODE, MEMORY};
    let param = match a.param {
        CODE => "code".to_owned(),
        MEMORY => "memory".to_owned(),
        n => n.to_string(),
    };
    let dynamic = a.dynamic.map_or("-".to_owned(), |v| format!("v{}", v.0));
    format!("{param}+{:x}+{dynamic}", a.offset)
}

fn source(s: &victory_shader::resource::Source, out: &mut String) {
    use victory_shader::resource::Source;
    let list = |args: &[Source], out: &mut String| {
        out.push('[');
        for (i, a) in args.iter().enumerate() {
            if i > 0 {
                out.push(',');
            }
            source(a, out);
        }
        out.push(']');
    };
    match s {
        Source::Loaded { load, first } => {
            let _ = write!(out, "L{}.{}+{first}", load.0, load.1);
        }
        Source::Fetched { load, first } => {
            let _ = write!(out, "F{}.{}+{first}", load.0, load.1);
        }
        Source::Built { address, bits, words } => {
            let _ = write!(out, "B({} {bits:x} {:x} {:x})", addr(address), words[0], words[1]);
        }
        Source::Constant(c) => {
            out.push_str("K[");
            for (i, w) in c.iter().enumerate() {
                let _ = write!(out, "{}{w:x}", if i > 0 { "," } else { "" });
            }
            out.push(']');
        }
        Source::Phi { block, args } => {
            let _ = write!(out, "P{block}");
            list(args, out);
        }
        Source::Select { scc, args } => {
            let _ = write!(out, "S{}", scc.0);
            list(args, out);
        }
        Source::Lanes { mask, args } => {
            let _ = write!(out, "M{}", mask.0);
            list(args, out);
        }
        Source::LanePhi { block, args } => {
            let _ = write!(out, "Q{block}[");
            for (i, a) in args.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                match a {
                    Some(a) => source(a, out),
                    None => out.push('-'),
                }
            }
            out.push(']');
        }
    }
}

  fn resource(data: &[u8], out: &mut String) {
    use victory_shader::resource::{BvhNodes, Role};
    let Some(p) = picked(data, out) else { return };
    let program = match victory_shader::spill::build(&p) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            return;
        }
        Ok(program) => program,
    };
    let st = victory_shader::structure::structure(&program);
    if let Err(e) = &st {
        let _ = writeln!(out, " st err {e}");
    }
    let r = match victory_shader::resource::track(&program, &p.layout, p.stage, st.as_ref().ok()) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            return;
        }
        Ok(r) => r,
    };
    for (site, a) in &r.scalar {
        let _ = writeln!(out, " scalar {}.{} {}", site.0, site.1, addr(a));
    }
    for d in &r.descriptors {
        let role = match d.role {
            Role::Resource => "r",
            Role::Sampler => "s",
        };
        let _ = write!(out, " desc {}.{} {role} ", d.site.0, d.site.1);
        source(&d.source, out);
        out.push('\n');
    }
    for v in &r.root_indexed_views {
        let _ = writeln!(
            out,
            " view {} {} {} {:x} {}",
            v.table_slot, v.constant_slot, v.constant_dword, v.byte_offset, v.shift
        );
    }
    for (v, param, dword) in &r.params {
        let _ = writeln!(out, " param v{} {param} {dword}", v.0);
    }
    for s in &r.lane_copied {
        let _ = writeln!(out, " copied {}.{}", s.0, s.1);
    }
    for s in &r.waterfall.compares {
        let _ = writeln!(out, " wf compare {}.{}", s.0, s.1);
    }
    for v in &r.waterfall.reads {
        let _ = writeln!(out, " wf read v{}", v.0);
    }
    for s in &r.waterfall.loads {
        let _ = writeln!(out, " wf load {}.{}", s.0, s.1);
    }
    for (s, b) in &r.bvh {
        match b {
            BvhNodes::Resource => {
                let _ = writeln!(out, " bvh {}.{} resource", s.0, s.1);
            }
            BvhNodes::Memory { word1, word3 } => {
                let _ = writeln!(out, " bvh {}.{} memory {word1:x} {word3:x}", s.0, s.1);
            }
        }
    }
}

fn usage(u: victory_shader::native::Usage) -> String {
    match u {
        victory_shader::native::Usage::Generic => "g".to_owned(),
        victory_shader::native::Usage::ClipDistance => "c".to_owned(),
        victory_shader::native::Usage::Other(n) => format!("o{n}"),
    }
}

 fn io(data: &[u8], out: &mut String) {
    use victory_shader::io::{Input, Interp, Target};
    let Some(p) = picked(data, out) else { return };
    let program = match victory_shader::spill::build(&p) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            return;
        }
        Ok(program) => program,
    };
    let io = match victory_shader::io::raise(&program, &p) {
        Err(e) => {
            let _ = writeln!(out, " err {e}");
            return;
        }
        Ok(io) => io,
    };
    for (v, i) in &io.inputs {
        let text = match i {
            Input::Attribute { row, channel } => format!("attr {row} {channel}"),
            Input::Barycentric { kind, index } => format!("bary {kind:?} {index}"),
            other => format!("{other:?}"),
        };
        let _ = writeln!(out, " input v{} {text}", v.0);
    }
    let attr = |a: &victory_shader::io::Attribute| format!("{} {} {} {}", usage(a.usage), a.row, a.channel, a.high);
    for (s, i) in &io.interps {
        let text = match i {
            Interp::Interpolated { attribute, barycentric, first } => {
                let f: Vec<String> = first.iter().map(|s| format!("{}.{}", s.0, s.1)).collect();
                format!("interp {} {barycentric:?} [{}]", attr(attribute), f.join(","))
            }
            Interp::Vertex { attribute, vertex } => format!("vertex {} {vertex}", attr(attribute)),
            Interp::Weighted { attribute } => format!("weighted {}", attr(attribute)),
        };
        let _ = writeln!(out, " interp {}.{} {text}", s.0, s.1);
    }
    for e in &io.exports {
        let target = match e.target {
            Target::Color(n) => format!("color{n}"),
            Target::Depth => "depth".to_owned(),
            Target::Null => "null".to_owned(),
            Target::Position(n) => format!("pos{n}"),
            Target::Param { usage: u, row } => format!("param {} {row}", usage(u)),
            Target::Unread => "unread".to_owned(),
            Target::Primitive => "prim".to_owned(),
            Target::PrimitiveParam { usage: u, row } => format!("primparam {} {row}", usage(u)),
        };
        let values: Vec<String> = e.values.iter().map(|v| v.map_or("-".to_owned(), |v| format!("v{}", v.0))).collect();
        let _ = writeln!(
            out,
            " export {}.{} {target} {} {} {}",
            e.site.0,
            e.site.1,
            values.join(","),
            e.compressed,
            e.done
        );
    }
    if let Some(n) = io.initialized_sgprs {
        let _ = writeln!(out, " sgprs {n}");
    }
}

pub fn run(stage: &str, dir: &Path, out: &Path, full: bool, only: Option<&Path>) {
    let names: Option<std::collections::HashSet<String>> =
        only.map(|p| std::fs::read_to_string(p).unwrap().lines().map(|l| l.trim().to_owned()).collect());
    let mut file = std::io::BufWriter::new(std::fs::File::create(out).unwrap());
    for f in crate::corpus_files(dir) {
        let name = f.file_stem().unwrap().to_string_lossy().into_owned();
        if names.as_ref().is_some_and(|n| !n.contains(&name)) {
            continue;
        }
        let data = std::fs::read(&f).unwrap();
        let mut text = String::new();
        match stage {
            "native" => native(&data, &mut text),
            "ir" => ir(&data, &mut text),
            "spill" => spill(&data, &mut text),
            "structure" => structure(&data, &mut text),
            "resource" => resource(&data, &mut text),
            "io" => io(&data, &mut text),
            _ => {
                eprintln!("no stage {stage}");
                std::process::exit(2);
            }
        }
        use std::io::Write as _;
        if full {
            let _ = writeln!(file, "{name}");
            file.write_all(text.as_bytes()).unwrap();
        } else {
            let _ = writeln!(file, "{name} {}", &blake3::hash(text.as_bytes()).to_hex()[..16]);
        }
    }
}
