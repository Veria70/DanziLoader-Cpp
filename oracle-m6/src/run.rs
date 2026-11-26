// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::{CStr, CString, c_char};
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use victory_loader::{
    BindReport, ImportResolver, Provenance, Resolution, UnimplementedPolicy, WithTitleDlls,
};
use victory_pe::ImportSymbol;
use windows_sys::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryA};

static OUT: OnceLock<Mutex<PathBuf>> = OnceLock::new();

fn out_line(line: &str) {
    let path = OUT.get().unwrap().lock().unwrap_or_else(|e| e.into_inner());
    let mut f = std::fs::OpenOptions::new().append(true).create(true).open(&*path).unwrap();
    let _ = writeln!(f, "{line}");
}

extern "system" fn harness_log(text: *const c_char) {

    let t = unsafe { CStr::from_ptr(text) }.to_string_lossy();
    out_line(&format!("log {t}"));
}

extern "system" fn harness_value(x: u32) -> u32 {
    x.wrapping_add(1000)
}

static MODE: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

extern "system" fn harness_mode() -> u32 {
    MODE.load(std::sync::atomic::Ordering::Relaxed)
}

struct TestResolver;

impl ImportResolver for TestResolver {
    fn resolve(&self, dll: &str, symbol: &ImportSymbol) -> Resolution {
        let l = dll.to_ascii_lowercase();
        if l == "fixture_host.dll" {
            return match symbol {
                ImportSymbol::Name { name, .. } if name == "Log" => Resolution::Found {
                    address: harness_log as *const () as usize,
                    provenance: Provenance::Hle,
                },
                ImportSymbol::Name { name, .. } if name == "Mode" => Resolution::Found {
                    address: harness_mode as *const () as usize,
                    provenance: Provenance::Hle,
                },
                ImportSymbol::Name { name, .. } if name == "Value" => Resolution::Found {
                    address: harness_value as *const () as usize,
                    provenance: Provenance::Hle,
                },
                _ => Resolution::Missing,
            };
        }
        if l.starts_with("fixture_") {
            return Resolution::Missing;
        }
        let Ok(name) = CString::new(dll) else {
            return Resolution::Missing;
        };

        let h = unsafe { LoadLibraryA(name.as_ptr().cast()) };
        if h.is_null() {
            return Resolution::Missing;
        }

        let p = match symbol {
            ImportSymbol::Name { name, .. } => match CString::new(name.as_str()) {
                Ok(n) => unsafe { GetProcAddress(h, n.as_ptr().cast()) },
                Err(_) => None,
            },
            ImportSymbol::Ordinal(o) => unsafe { GetProcAddress(h, *o as usize as _) },
        };
        match p {
            Some(f) => Resolution::Found {
                address: f as *const () as usize,
                provenance: Provenance::Host,
            },
            None => Resolution::Missing,
        }
    }
}

fn report_line(who: &str, r: &BindReport) -> String {
    let missing: Vec<String> = r.missing.iter().map(|m| format!("{}!{}", m.dll, m.symbol)).collect();
    format!(
        "bind {who}: hle={} host={} title={} missing=[{}]",
        r.hle,
        r.host,
        r.title,
        missing.join(", ")
    )
}

pub fn run(title_dir: &str, exe: &str, cache: &str, out: &str, mode: &str) -> i32 {
    let trap = mode == "trap";
    if let Some(n) = mode.strip_prefix("crash") {
        MODE.store(n.parse().unwrap_or(0), std::sync::atomic::Ordering::Relaxed);
        victory_loader::log_crashes_to(PathBuf::from(format!("{out}.crash")));
        victory_loader::report_crashes();
    }
    let policy = if trap {
        victory_loader::log_crashes_to(PathBuf::from(format!("{out}.crash")));
        UnimplementedPolicy::Trap
    } else {
        UnimplementedPolicy::ReturnZero
    };
    let _ = OUT.set(Mutex::new(PathBuf::from(out)));
    std::fs::write(out, "").unwrap();
    let result = (|| -> anyhow::Result<()> {
        let title_dir = Path::new(title_dir);
        let cache = Path::new(cache);
        let original = std::fs::read(title_dir.join(exe))?;
        let prepared = victory_loader::prepare(&original)?;
        let image = Arc::new(victory_loader::load(&prepared, cache)?);
        let dlls = victory_loader::load_title_dlls(image.info(), title_dir, cache, &|n: &str| n.starts_with("fixture_dll"))?;
        let host = TestResolver;
        let resolver = WithTitleDlls { inner: &host, dlls: &dlls };
        for (name, dll) in &dlls {
            out_line(&report_line(name, &dll.bind_imports(&resolver, policy)?));
        }
        out_line(&report_line("main", &image.bind_imports(&resolver, policy)?));

        let thread = unsafe { image.clone().spawn_main(dlls) }?;
        let code = thread.join().map_err(|_| anyhow::anyhow!("the title's main thread panicked"))??;
        out_line(&format!("exit {code}"));
        out_line(&format!("unimplemented calls {}", victory_loader::unimplemented_call_count()));
        Ok(())
    })();
    if let Err(e) = result {
        out_line(&format!("error {e:#}"));
        return 1;
    }
    out_line("process exit");
    0
}
