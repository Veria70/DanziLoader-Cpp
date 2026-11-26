// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;
use std::ffi::c_void;
use std::fmt::Write as _;
use std::sync::atomic::{AtomicU32, Ordering};

use victory_hle::modules::xasync::{self, AsyncBlock, ProviderData};

fn bump(at: *mut u8) {

    unsafe { (*at.cast::<AtomicU32>()).fetch_add(1, Ordering::SeqCst) };
}
extern "system" fn cb_count(context: *mut c_void, canceled: bool) {
    bump(context.cast());
    if canceled {

        bump(unsafe { context.cast::<u8>().add(4) });
    }
}
extern "system" fn cb_term(context: *mut c_void) {

    bump(unsafe { context.cast::<u8>().add(8) });
}
extern "system" fn cb_monitor(context: *mut c_void, _queue: *const c_void, port: u32) {

    unsafe {
        bump(context.cast::<u8>().add(12));
        context.cast::<u8>().add(20).cast::<u32>().write_unaligned(port);
    }
}
extern "system" fn cb_complete(block: *mut AsyncBlock) {

    bump(unsafe { (*block).context.cast::<u8>().add(16) });
}
extern "system" fn cb_work(_: *mut AsyncBlock) -> i32 {
    7
}
extern "system" fn cb_provider(op: u32, data: *const ProviderData) -> i32 {

    let data = unsafe { &*data };
    match op {
        0 => xasync::XAsyncSchedule(data.block, 0),
        1 => {
            xasync::XAsyncComplete(data.block, 0, 8);
            0
        }

        2 => unsafe {
            data.buffer.cast::<u64>().write_unaligned(0x1122_3344_5566_7788);
            0
        },
        _ => 0,
    }
}

extern "system" fn cb_idle(_: u32, _: *const ProviderData) -> i32 {
    0
}

 static LOG: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

#[repr(C)]
struct ContainerInfo {
    name: *const std::ffi::c_char,
    display_name: *const std::ffi::c_char,
    blob_count: u32,
    total_size: u64,
    last_modified: i64,
    needs_sync: bool,
}
#[repr(C)]
struct BlobInfo {
    name: *const std::ffi::c_char,
    size: u32,
}
fn cstr(p: *const std::ffi::c_char) -> String {

    unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned()
}
fn note_container(info: *const ContainerInfo) {

    let i = unsafe { &*info };
    let line = format!(
        "container {} [{}] blobs {} size {} sync {} dated {}",
        cstr(i.name), cstr(i.display_name), i.blob_count, i.total_size, i.needs_sync, i.last_modified > 0
    );
    LOG.lock().unwrap().push(line);
}
extern "system" fn cb_container(info: *const ContainerInfo, _: *mut c_void) -> bool {
    note_container(info);
    true
}
extern "system" fn cb_container1(info: *const ContainerInfo, _: *mut c_void) -> bool {
    note_container(info);
    false
}
fn note_blob(info: *const BlobInfo) {

    let i = unsafe { &*info };
    LOG.lock().unwrap().push(format!("blob {} {}", cstr(i.name), i.size));
}
extern "system" fn cb_blob(info: *const BlobInfo, _: *mut c_void) -> bool {
    note_blob(info);
    true
}
extern "system" fn cb_blob1(info: *const BlobInfo, _: *mut c_void) -> bool {
    note_blob(info);
    false
}

 fn tree(dir: &std::path::Path, base: &std::path::Path, out: &mut Vec<String>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        let rel = p.strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/");
        if p.is_dir() {
            out.push(format!("{rel}/"));
            tree(&p, base, out);
        } else {
            out.push(format!("{rel} {}", std::fs::metadata(&p).map_or(0, |m| m.len())));
        }
    }
}

 fn var(vars: &HashMap<String, u64>, v: &str) -> u64 {
    match v.split_once('+') {
        Some((name, off)) => {
            let off = match off.strip_prefix("0x") {
                Some(h) => u64::from_str_radix(h, 16).unwrap(),
                None => off.parse().unwrap(),
            };
            vars[name].wrapping_add(off)
        }
        None => vars[v],
    }
}

fn builtin(name: &str) -> Option<u64> {
    Some(match name {
        "@container" => cb_container as *const () as u64,
        "@container1" => cb_container1 as *const () as u64,
        "@blob" => cb_blob as *const () as u64,
        "@blob1" => cb_blob1 as *const () as u64,
        "@count" => cb_count as *const () as u64,
        "@term" => cb_term as *const () as u64,
        "@monitor" => cb_monitor as *const () as u64,
        "@complete" => cb_complete as *const () as u64,
        "@work" => cb_work as *const () as u64,
        "@provider" => cb_provider as *const () as u64,
        "@idle" => cb_idle as *const () as u64,
        _ => return None,
    })
}

fn parse_guid(s: &str) -> [u8; 16] {
    let h: String = s.chars().filter(|c| *c != '-').collect();
    let byte = |i: usize| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).unwrap();
    let mut g = [0u8; 16];
    let d1 = u32::from_str_radix(&h[0..8], 16).unwrap();
    let d2 = u16::from_str_radix(&h[8..12], 16).unwrap();
    let d3 = u16::from_str_radix(&h[12..16], 16).unwrap();
    g[0..4].copy_from_slice(&d1.to_le_bytes());
    g[4..6].copy_from_slice(&d2.to_le_bytes());
    g[6..8].copy_from_slice(&d3.to_le_bytes());
    for i in 8..16 {
        g[i] = byte(i);
    }
    g
}

use windows_sys::Win32::Foundation::{GetLastError, SetLastError};
use windows_sys::Win32::System::LibraryLoader::{
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    GetModuleFileNameW, GetModuleHandleExW, GetModuleHandleW,
};

fn module_file(module: usize) -> String {
    let mut path = [0u16; 260];

    let len = unsafe { GetModuleFileNameW(module as _, path.as_mut_ptr(), 260) } as usize;
    let p = String::from_utf16_lossy(&path[..len]);
    p.rsplit('\\').next().unwrap_or(&p).to_ascii_lowercase()
}

  fn show(v: u64) -> String {
    if v == 0 {
        return "0".into();
    }
    for m in victory_hle::registry() {
        if m as *const _ as u64 == v {
            return format!("pseudo {}", m.name);
        }
        for e in &m.exports {
            if e.address as u64 == v {
                return format!("export {}!{}", m.name, e.name);
            }
        }
    }
    let mut module = std::ptr::null_mut();

    let found = unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            v as usize as *const u16,
            &mut module,
        )
    } != 0;

    let own = unsafe { GetModuleHandleW(std::ptr::null()) } as u64;
    if found && module as u64 == own {
        return "self".into();
    }
    if found {
        let base = module as u64;
        let file = module_file(module as usize);
        return if base == v { format!("module {file}") } else { format!("in {file}+{:#x}", v - base) };
    }
    if v <= u64::from(u32::MAX) { v.to_string() } else { format!("{v:#x}") }
}

fn host_module(name: &str) -> usize {
    let w: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();

    unsafe { GetModuleHandleW(w.as_ptr()) as usize }
}

fn esc(s: &str) -> String {
    let mut out = String::new();
    for &b in s.as_bytes() {
        if (0x21..=0x7e).contains(&b) && b != b'\\' {
            out.push(b as char);
        } else {
            let _ = write!(out, "\\x{b:02x}");
        }
    }
    out
}

pub fn run(scenario: &str, out_path: &str) {
    let text = std::fs::read_to_string(scenario).unwrap();
    let mut bufs: HashMap<String, Vec<u8>> = HashMap::new();
    let mut out = String::new();

    let mut last = 0u64;
    let mut vars: HashMap<String, u64> = HashMap::new();

    let mut drive = String::from("?");
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let t: Vec<&str> = line.split_whitespace().collect();
        let rest = |k: usize| line.splitn(k + 1, char::is_whitespace).nth(k).unwrap_or("").trim().to_owned();
        let _ = write!(out, "#{} {line}\n", n + 1);
        match t[0] {
            "buf" => {
                bufs.insert(t[1].into(), vec![0; t[2].parse().unwrap()]);
            }
            "mount" => {
                drive = victory_hle::modules::files::mount(std::path::Path::new(t[1])).map_or("?".into(), String::from);
            }
            "mount-at" => victory_hle::modules::files::mount_at(t[1].chars().next().unwrap(), std::path::Path::new(t[2])),
            "str" => {
                let mut b = rest(2).replace("%D", &drive).into_bytes();
                b.push(0);
                bufs.insert(t[1].into(), b);
            }
            "wstr" => {
                let b: Vec<u8> = rest(2).replace("%D", &drive).encode_utf16().chain(Some(0)).flat_map(u16::to_le_bytes).collect();
                bufs.insert(t[1].into(), b);
            }
            "title" => victory_hle::runtime::set_title(victory_hle::runtime::Title {
                id: u32::from_str_radix(t[1].trim_start_matches("0x"), 16).unwrap(),
                store_id: if t[2] == "-" { String::new() } else { t[2].into() },
                storage: if t[3] == "-" { std::path::PathBuf::new() } else { t[3].into() },
                storage_bytes: t[4].parse().unwrap(),
            }),
            "mkdir" => std::fs::create_dir_all(t[1]).unwrap(),
            "mkfile" => std::fs::write(t[1], rest(2)).unwrap(),
            "tree" => {
                let mut lines = Vec::new();
                tree(std::path::Path::new(t[1]), std::path::Path::new(t[1]), &mut lines);
                lines.sort();
                for l in lines {
                    let _ = writeln!(out, "  {l}");
                }
            }
            "log" => {
                for l in LOG.lock().unwrap().drain(..) {
                    let _ = writeln!(out, "  {}", esc(&l));
                }
            }
            "main-image" => victory_hle::runtime::set_main_image(host_module(t[1])),
            "main-image-none" => victory_hle::runtime::set_main_image(0),
            "image-path" => victory_hle::runtime::set_image_path(host_module(t[1]), std::path::Path::new(&rest(2))),
            "guid" => {
                bufs.insert(t[1].into(), parse_guid(t[2]).to_vec());
            }
            "load" => {
                let b = &bufs[t[2]];
                let at: usize = t[3].parse().unwrap();
                vars.insert(t[1].into(), u64::from_le_bytes(b[at..at + 8].try_into().unwrap()));
            }
            "let" => {
                vars.insert(t[1].into(), last);
            }
            "sleep" => std::thread::sleep(std::time::Duration::from_millis(t[1].parse().unwrap())),
            "store" => {
                let a = t[3];
                let v = if let Some(name) = a.strip_prefix('&') {
                    bufs.get_mut(name).unwrap().as_mut_ptr() as u64
                } else if let Some(f) = builtin(a) {
                    f
                } else if let Some(v) = a.strip_prefix('$') {
                    var(&vars, v)
                } else if let Some(h) = a.strip_prefix("0x") {
                    u64::from_str_radix(h, 16).unwrap()
                } else {
                    a.parse().unwrap()
                };
                let at: usize = t[2].parse().unwrap();
                bufs.get_mut(t[1]).unwrap()[at..at + 8].copy_from_slice(&v.to_le_bytes());
            }
            "dump" => {
                let b = &bufs[t[1]];
                let n: usize = t[2].parse().unwrap();
                let from: usize = t.get(3).map_or(0, |f| f.parse::<usize>().unwrap().min(b.len()));
                let hex: Vec<String> = b[from..(from + n).min(b.len())].iter().map(|x| format!("{x:02x}")).collect();
                let _ = writeln!(out, "  {}", hex.join(""));
            }
            "text" => {
                let b = &bufs[t[1]];
                let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
                let _ = writeln!(out, "  {}", esc(&String::from_utf8_lossy(&b[..end])));
            }
            "wtext" => {
                let b = &bufs[t[1]];
                let w: Vec<u16> = b.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).take_while(|&c| c != 0).collect();
                let _ = writeln!(out, "  {}", esc(&String::from_utf16_lossy(&w)));
            }
            "call" | "vcall" => {


                let (name, width) = match t[2].split_once(':') {
                    Some((n, w)) => (n, w.parse::<u32>().unwrap()),
                    None => (t[2], 64),
                };
                let mut args = [0u64; 8];
                let mut shift = 0usize;
                let address = if t[0] == "vcall" {
                    let object = vars[t[1].trim_start_matches('$')];
                    let slot = u64::from_str_radix(name.trim_start_matches("0x"), 16).unwrap();
                    args[0] = object;
                    shift = 1;

                    unsafe { *((*(object as *const u64)) as *const u64).add((slot / 8) as usize) as usize }
                } else {
                    match victory_hle::registry()
                        .iter()
                        .filter(|m| m.serves(t[1]))
                        .flat_map(|m| m.exports.iter())
                        .find(|e| e.name == name)
                        .map(|e| e.address)
                    {
                        Some(a) => a,
                        None => {
                            let _ = writeln!(out, "  no such export");
                            continue;
                        }
                    }
                };
                for (k, a) in t[3..].iter().enumerate() {
                    let k = k + shift;
                    args[k] = if let Some(name) = a.strip_prefix('&') {
                        let (name, off) = name.split_once('+').map_or((name, 0usize), |(n, o)| (n, o.parse().unwrap()));
                        bufs.get_mut(name).unwrap().as_mut_ptr() as u64 + off as u64
                    } else if let Some(f) = builtin(a) {
                        f
                    } else if let Some(v) = a.strip_prefix('$').filter(|v| *v != "last") {
                        var(&vars, v)
                    } else if *a == "null" {
                        0
                    } else if *a == "$last" {
                        last
                    } else if let Some(h) = a.strip_prefix("0x") {
                        u64::from_str_radix(h, 16).unwrap()
                    } else if let Some(neg) = a.strip_prefix('-') {
                        neg.parse::<u64>().unwrap().wrapping_neg()
                    } else {
                        a.parse().unwrap()
                    };
                }
                type F = unsafe extern "system" fn(u64, u64, u64, u64, u64, u64, u64, u64) -> u64;

                let f: F = unsafe { std::mem::transmute(address) };

                unsafe { SetLastError(0) };
                let r = unsafe { f(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]) };

                let err = unsafe { GetLastError() };
                last = r;
                let shown = match width {
                    0 => "-".to_string(),
                    8 => show(r & 0xFF),
                    32 => show(r & 0xFFFF_FFFF),
                    _ => show(r),
                };


                if t[0] == "vcall" {
                    let _ = writeln!(out, "  -> {shown}");
                } else {
                    let _ = writeln!(out, "  -> {shown} err {err}");
                }
            }
            other => panic!("unknown command {other}"),
        }
    }
    std::fs::write(out_path, out).unwrap();
}
