// SPDX-License-Identifier: GPL-3.0-or-later

use std::fmt::Write as _;

use victory_pe::implib::{ImportLibrary, LibImportKind};
use victory_pe::{ImageInfo, ImportSymbol};

mod hle_run;
mod run;

 fn esc(s: &str) -> String {
    let mut out = String::new();
    for &b in s.as_bytes() {
        if (0x21..=0x7e).contains(&b) && b != b'\\' {
            out.push(b as char);
        } else {
            let _ = write!(out, "\\x{b:02x}");
        }
    }
    if out.is_empty() {
        out.push_str("\\e");
    }
    out
}

fn sym(s: &ImportSymbol) -> String {
    match s {
        ImportSymbol::Name { hint, name } => format!("n:{}:{hint}", esc(name)),
        ImportSymbol::Ordinal(o) => format!("o:{o}"),
    }
}

fn image_block(out: &mut String, data: &[u8]) {
    let info = match ImageInfo::parse(data) {
        Ok(i) => i,
        Err(e) => {
            let _ = writeln!(out, "err {}", esc(&e.to_string()));
            return;
        }
    };
    let _ = writeln!(
        out,
        "ok m={:04x} c={:04x} ts={:08x} sub={} {} dc={:04x} base={:x} size={:x} entry={:x} stack={:x}/{:x} reloc={} exc={} lc={} dll={} imports={}",
        info.machine,
        info.characteristics,
        info.timestamp,
        info.subsystem,
        info.subsystem_name(),
        info.dll_characteristics,
        info.image_base,
        info.size_of_image,
        info.entry_rva,
        info.stack_reserve,
        info.stack_commit,
        u8::from(info.has_relocations),
        u8::from(info.has_exception_table),
        u8::from(info.has_load_config),
        u8::from(info.is_dll()),
        info.import_count()
    );
    for s in &info.sections {
        let _ = writeln!(
            out,
            "sec {} {:x} {:x} {:x} {:x} {:08x} off={}",
            esc(&s.name),
            s.rva,
            s.virtual_size,
            s.file_offset,
            s.file_size,
            s.characteristics,


            [s.rva, s.rva.wrapping_add(0x10)]
                .iter()
                .map(|&r| info.rva_to_file_offset(r).map_or("-".to_string(), |o| format!("{o:x}")))
                .collect::<Vec<_>>()
                .join(",")
        );
    }
    let _ = writeln!(
        out,
        "entry-off {}",
        info.rva_to_file_offset(info.entry_rva).map_or("-".to_string(), |o| format!("{o:x}"))
    );
    for (tag, list) in [("imp", &info.imports), ("dimp", &info.delay_imports)] {
        for m in list {
            let _ = writeln!(out, "{tag} {} {}", esc(&m.dll), m.entries.len());
            for e in &m.entries {
                let _ = writeln!(out, " i {} {:x} {}", sym(&e.symbol), e.iat_slot_rva, esc(&e.symbol.to_string()));
            }
        }
    }
    for e in &info.exports {
        let _ = writeln!(
            out,
            "exp {} {} {} {}",
            e.name.as_deref().map_or("-".to_string(), esc),
            e.ordinal,
            e.rva.map_or("-".to_string(), |r| format!("{r:x}")),
            e.forward.as_deref().map_or("-".to_string(), esc)
        );
    }
    if let Some(t) = &info.tls {
        let rvas: Vec<String> = t.callback_rvas.iter().map(|r| format!("{r:x}")).collect();
        let _ = writeln!(out, "tls {:x} {:x} [{}]", t.directory_rva, t.callbacks_va, rvas.join(","));
    }
    if let Some(cv) = &info.codeview {
        let guid: String = cv.guid.iter().map(|b| format!("{b:02x}")).collect();
        let _ = writeln!(out, "cv {} {guid} {} {}", esc(&cv.pdb_path), cv.age, cv.symbol_key());
    }
}

fn lib_block(out: &mut String, data: &[u8]) {
    match ImportLibrary::parse(data) {
        Ok(lib) => {
            let _ = writeln!(out, "lib other={} imports={}", lib.other_members, lib.imports.len());
            for i in &lib.imports {
                let kind = match i.kind {
                    LibImportKind::Code => "code",
                    LibImportKind::Data => "data",
                    LibImportKind::Const => "const",
                };
                let _ = writeln!(out, "li {} {} {kind}", esc(&i.dll), sym(&i.export));
            }
        }
        Err(e) => {
            let _ = writeln!(out, "err {}", esc(&e.to_string()));
        }
    }
}

fn main() {

    victory_core::logging::init("info");
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("pe") if args.len() >= 4 => {
            let list = std::fs::read_to_string(&args[2]).unwrap();
            let mut out = String::new();
            for path in list.lines().map(str::trim).filter(|l| !l.is_empty()) {
                let _ = writeln!(out, "== {}", esc(path));
                let Ok(data) = std::fs::read(path) else {
                    let _ = writeln!(out, "unreadable");
                    continue;
                };
                if path.to_ascii_lowercase().ends_with(".lib") {
                    lib_block(&mut out, &data);
                } else {
                    image_block(&mut out, &data);
                }
            }
            std::fs::write(&args[3], out).unwrap();
        }
        Some("prepare") if args.len() >= 4 => {
            let list = std::fs::read_to_string(&args[2]).unwrap();
            let mut out = String::new();
            for path in list.lines().map(str::trim).filter(|l| !l.is_empty()) {
                let _ = writeln!(out, "== {}", esc(path));
                let Ok(data) = std::fs::read(path) else {
                    let _ = writeln!(out, "unreadable");
                    continue;
                };
                match victory_loader::prepare(&data) {
                    Ok(p) => {
                        let _ = writeln!(out, "ok {} {} {}", blake3::hash(&p.bytes).to_hex(), p.file_name(), p.bytes.len());
                    }
                    Err(e) => {
                        let _ = writeln!(out, "err {}", esc(&e.to_string()));
                    }
                }
            }
            std::fs::write(&args[3], out).unwrap();
        }
        Some("policy") if args.len() >= 4 => {
            let list = std::fs::read_to_string(&args[2]).unwrap();
            let mut out = String::new();
            for path in list.lines().map(str::trim).filter(|l| !l.is_empty()) {
                match victory_core::ensure_outside_repo(std::path::Path::new(path)) {
                    Ok(()) => {
                        let _ = writeln!(out, "{} ok", esc(path));
                    }
                    Err(e) => {
                        let _ = writeln!(out, "{} err {}", esc(path), esc(&e.to_string()));
                    }
                }
            }
            std::fs::write(&args[3], out).unwrap();
        }
        Some("run") if args.len() >= 6 => {


            let code = run::run(&args[2], &args[3], &args[4], &args[5], args.get(6).map_or("", String::as_str));
            if code != 0 {
                std::process::exit(code);
            }
        }
        Some("hle-run") if args.len() >= 4 => hle_run::run(&args[2], &args[3]),
        Some("hle-registry") if args.len() >= 3 => {
            let mut out = String::new();
            for m in victory_hle::registry() {
                let _ = writeln!(out, "module {} dlls={}", m.name, m.dlls.join(","));
                for e in &m.exports {
                    let _ = writeln!(out, " export {}", e.name);
                }
            }
            std::fs::write(&args[2], out).unwrap();
        }
        Some("hle-classify") if args.len() >= 4 => {
            let list = std::fs::read_to_string(&args[2]).unwrap();
            let resolver = victory_hle::HleResolver::default();
            let mut out = String::new();
            for path in list.lines().map(str::trim).filter(|l| !l.is_empty()) {
                let _ = writeln!(out, "== {}", esc(path));
                let Ok(data) = std::fs::read(path) else {
                    let _ = writeln!(out, "unreadable");
                    continue;
                };
                let Ok(info) = ImageInfo::parse(&data) else {
                    let _ = writeln!(out, "not an image");
                    continue;
                };
                for m in info.imports.iter().chain(&info.delay_imports) {
                    for e in &m.entries {
                        let class = match resolver.classify(&m.dll, &e.symbol) {
                            Some(victory_loader::Provenance::Hle) => "hle",
                            Some(victory_loader::Provenance::Host) => "host",
                            Some(victory_loader::Provenance::Title) => "title",
                            None => "missing",
                        };
                        let _ = writeln!(out, "{}!{} {class}", esc(&m.dll), esc(&e.symbol.to_string()));
                    }
                }
            }
            std::fs::write(&args[3], out).unwrap();
        }
        _ => {
            eprintln!("usage: m6-oracle pe|prepare|policy <list.txt> <out.txt> | run <title dir> <exe> <cache dir> <out.txt>");
            std::process::exit(2);
        }
    }
}
