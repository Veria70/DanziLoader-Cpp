// SPDX-License-Identifier: GPL-3.0-or-later

mod dump;
mod irdump;
mod stdump;

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use victory_shader::Container;

fn walk(path: &Path, files: &mut Vec<PathBuf>) {
    if path.is_dir() {
        let Ok(entries) = std::fs::read_dir(path) else { return };
        let mut all: Vec<PathBuf> = entries.filter_map(Result::ok).map(|e| e.path()).collect();
        all.sort();
        for p in all {
            walk(&p, files);
        }
    } else {
        files.push(path.to_path_buf());
    }
}

 fn containers(data: &[u8]) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    if victory_game::sgd5::is_sgd5(data) {
        out.extend(victory_game::sgd5::records(data).filter_map(|r| r.unpack()));
    }
    for offset in memchr::memmem::find_iter(data, b"DXBC") {
        let rest = &data[offset..];
        if Container::parse(rest).is_err() {
            continue;
        }
        let total = u32::from_le_bytes(rest[0x18..0x1c].try_into().unwrap()) as usize;
        out.push(rest[..total].to_vec());
    }
    out
}

fn corpus(out: &Path, inputs: &[PathBuf]) {
    std::fs::create_dir_all(out).unwrap();
    let mut files = Vec::new();
    for i in inputs {
        walk(i, &mut files);
    }
    let (mut found, mut written) = (0usize, 0usize);
    for f in files {
        let len = std::fs::metadata(&f).map_or(0, |m| m.len());
        if len == 0 || len > 2 << 30 {
            continue;
        }
        let Ok(data) = std::fs::read(&f) else { continue };
        for c in containers(&data) {
            found += 1;
            let name = blake3::hash(&c).to_hex()[..16].to_owned();
            let path = out.join(format!("{name}.dxbc"));
            if !path.exists() {
                std::fs::write(&path, &c).unwrap();
                written += 1;
            }
        }
        eprintln!("{}: {found} containers so far, {written} new", f.display());
    }
    println!("{found} containers, {written} written");
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

    fn result_line(name: &str, r: &Result<victory_shader::emit::HostShader, victory_shader::ShaderError>) -> String {
    match r {
        Err(e) => format!("{name} err {e}"),
        Ok(s) => {
            let mut l = format!("{name} ok {:?} dxil {} {}", s.stage, &blake3::hash(&s.dxil).to_hex()[..32], s.dxil.len());
            l.push_str(" ud");
            for u in &s.user_data {
                let _ = write!(l, " {}:{}:{:?}:{}", u.index, u.slot, u.root, u.dword);
            }
            l.push_str(" s");
            for w in &s.samplers {
                let _ = write!(l, " {:08x}{:08x}{:08x}{:08x}", w[0], w[1], w[2], w[3]);
            }
            l.push_str(" ss");
            for x in &s.static_samplers {
                let _ = write!(
                    l,
                    " {}:{}:{}:{}:{:08x}:{}:{}:{}:{:08x}:{:08x}",
                    x.filter,
                    x.address[0],
                    x.address[1],
                    x.address[2],
                    x.mip_lod_bias.to_bits(),
                    x.max_anisotropy,
                    x.comparison,
                    x.border,
                    x.min_lod.to_bits(),
                    x.max_lod.to_bits()
                );
            }
            let _ = write!(l, " cbv {} srv {} uav {}", hex(&s.root_cbvs), hex(&s.root_srvs), hex(&s.root_uavs));
            l.push_str(" riv");
            for v in &s.root_indexed_views {
                let _ = write!(l, " {}:{}:{}:{}:{}", v.table_slot, v.constant_slot, v.constant_dword, v.byte_offset, v.shift);
            }
            l
        }
    }
}

fn rect_line(r: &Result<Vec<u8>, victory_shader::ShaderError>) -> String {
    match r {
        Ok(gs) => format!("ok {} {}", &blake3::hash(gs).to_hex()[..32], gs.len()),
        Err(e) => format!("err {e}"),
    }
}

fn settings(args: &[String]) {
    let wave = args.first().and_then(|a| a.parse().ok()).unwrap_or(32);
    let limit = args.get(1).and_then(|a| a.parse().ok()).unwrap_or(999_999);
    victory_shader::emit::set_host_max_wave(wave);
    victory_shader::emit::set_view_heap_limit(limit);
}

pub fn corpus_files(dir: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    walk(dir, &mut files);
    files.retain(|f| f.extension().is_some_and(|e| e == "dxbc"));
    files
}

fn translate(dir: &Path, out: &Path, timed: bool) {
    let mut text = String::new();
    for f in corpus_files(dir) {
        let name = f.file_stem().unwrap().to_string_lossy().into_owned();
        let data = std::fs::read(&f).unwrap();
        if timed {
            let mut ns = Vec::new();
            for _ in 0..3 {
                let t = std::time::Instant::now();
                let r = victory_shader::emit::translate(&data);
                ns.push(t.elapsed().as_nanos() as u64);
                std::hint::black_box(r.is_ok());
            }
            ns.sort();
            let _ = writeln!(text, "{name} {}", ns[1]);
        } else {
            let r = victory_shader::emit::translate(&data);
            text.push_str(&result_line(&name, &r));
            text.push('\n');
        }
    }
    std::fs::write(out, text).unwrap();
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("corpus") if args.len() >= 4 => {
            let inputs: Vec<PathBuf> = args[3..].iter().map(PathBuf::from).collect();
            corpus(Path::new(&args[2]), &inputs);
        }
        Some(cmd @ ("translate" | "time")) if args.len() >= 4 => {
            settings(&args[4..]);
            translate(Path::new(&args[2]), Path::new(&args[3]), cmd == "time");
        }
        Some("rect") if args.len() >= 4 => {


            settings(&[]);
            let mut text = String::new();
            let mut first_vs: Option<Vec<u8>> = None;
            let mut pss = Vec::new();
            for f in corpus_files(Path::new(&args[2])) {
                let name = f.file_stem().unwrap().to_string_lossy().into_owned();
                let data = std::fs::read(&f).unwrap();
                let Ok(s) = victory_shader::emit::translate(&data) else { continue };
                match s.stage {
                    victory_shader::HwStage::Vs => {
                        let r = victory_shader::emit::rect_list_gs(&s.dxil, None);
                        let _ = writeln!(text, "{name} vs {}", rect_line(&r));
                        if first_vs.is_none() {
                            first_vs = Some(s.dxil.clone());
                        }
                    }
                    victory_shader::HwStage::Ps => pss.push((name, s.dxil)),
                    _ => {}
                }
            }
            if let Some(vs) = first_vs {
                for (name, ps) in pss {
                    let r = victory_shader::emit::rect_list_gs(&vs, Some(&ps));
                    let _ = writeln!(text, "{name} ps {}", rect_line(&r));
                }
            }
            std::fs::write(&args[3], text).unwrap();
        }
        Some("dxil") if args.len() >= 4 => {

            settings(&[]);
            let data = std::fs::read(&args[2]).unwrap();
            match victory_shader::emit::translate(&data) {
                Ok(s) => std::fs::write(&args[3], &s.dxil).unwrap(),
                Err(e) => {
                    eprintln!("{e}");
                    std::process::exit(1);
                }
            }
        }
        Some(cmd @ ("dump" | "hash")) if args.len() >= 5 => {

            let only = args.get(5).map(PathBuf::from);
            settings(&[]);
            dump::run(&args[2], Path::new(&args[3]), Path::new(&args[4]), cmd == "dump", only.as_deref());
        }
        _ => {
            eprintln!(
                "usage: shader-oracle corpus <out dir> <file or dir>... | translate|time <corpus dir> <out.txt> [wave] [view heap limit] | dump <stage> <corpus dir> <out.txt>"
            );
            std::process::exit(2);
        }
    }
}
