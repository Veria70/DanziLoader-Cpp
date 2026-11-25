
use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;
use std::sync::Arc;

use windows_sys::Win32::Foundation::{HANDLE, INVALID_HANDLE_VALUE};
use windows_sys::Win32::System::Diagnostics::Debug::ReadProcessMemory;
use windows_sys::Win32::System::Memory::*;
use windows_sys::Win32::System::Threading::GetCurrentProcess;

use crate::shadow::Shadow;

const BLOCK: usize = 64 << 10;
const KPAGE: usize = 4096;
const MEM_WRITE_WATCH: u32 = 0x20_0000;

struct Copy {
    name: String,
    shadow: Arc<Shadow>,
}

#[derive(Default)]
struct World {
         places: BTreeMap<String, usize>,
    place_len: BTreeMap<String, usize>,
    sections: BTreeMap<String, HANDLE>,
    copies: Vec<Copy>,
    log: String,
         quiet: bool,
         timing: String,
    op_number: usize,
              card_writes: Vec<(Arc<Shadow>, u64, Vec<u8>)>,
         flights: std::collections::VecDeque<crate::host::Flight>,
         range_set: crate::memcopy::RangeSet,
         buffers: BTreeMap<String, (usize, u64)>,
    next_resource: usize,
}

fn num(s: &str) -> Result<i64, String> {
    let (neg, body) = match s.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, s),
    };
    let v = match body.strip_prefix("0x") {
        Some(hex) => i64::from_str_radix(hex, 16),
        None => body.parse::<i64>(),
    }
    .map_err(|_| format!("not a number: {s}"))?;
    Ok(if neg { -v } else { v })
}

fn unum(s: &str) -> Result<usize, String> {
    let v = num(s)?;
    usize::try_from(v).map_err(|_| format!("negative: {s}"))
}

 fn pattern(seed: u32, i: usize) -> u8 {
    let mut x = seed.wrapping_mul(0x9E37_79B1) ^ (i as u32).wrapping_mul(0x85EB_CA77);
    x ^= x >> 15;
    x as u8
}

fn fnv64(bytes: &[u8]) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in bytes {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

  fn read_title(at: usize, len: usize) -> Vec<u8> {
    let mut out = vec![0u8; len];
    let mut done = 0;
    while done < len {
        let a = at + done;
        let n = ((a / KPAGE + 1) * KPAGE - a).min(len - done);
        let mut got = 0usize;

        let ok = unsafe { ReadProcessMemory(GetCurrentProcess(), a as *const _, out[done..].as_mut_ptr().cast(), n, &mut got) };
        if ok == 0 || got != n {
            out[done..done + n].fill(0);
        }
        done += n;
    }
    out
}

fn writable_page(at: usize) -> bool {

    let mut info: MEMORY_BASIC_INFORMATION = unsafe { std::mem::zeroed() };
    let n = unsafe { VirtualQuery(at as *const _, &mut info, size_of::<MEMORY_BASIC_INFORMATION>()) };
    n != 0 && info.State == MEM_COMMIT && (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) != 0
}

  fn title_write(at: usize, len: usize, seed: u32) -> (usize, usize) {
    let (mut written, mut skipped, mut i) = (0, 0, 0);
    while i < len {
        let a = at + i;
        let n = ((a / KPAGE + 1) * KPAGE - a).min(len - i);
        if writable_page(a) {
            for k in 0..n {


                unsafe { ((a + k) as *mut u8).write_volatile(pattern(seed, i + k)) };
            }
            written += n;
        } else {
            skipped += n;
        }
        i += n;
    }
    (written, skipped)
}

impl World {
    fn place(&self, name: &str) -> Result<usize, String> {
        self.places.get(name).copied().ok_or_else(|| format!("unknown place {name}"))
    }

    fn section(&self, name: &str) -> Result<HANDLE, String> {
        self.sections.get(name).copied().ok_or_else(|| format!("unknown section {name}"))
    }

    fn copy(&self, name: &str) -> Result<&Copy, String> {
        self.copies.iter().find(|c| c.name == name).ok_or_else(|| format!("unknown shadow {name}"))
    }

              fn declared(&self, t: &[&str]) -> Result<Vec<(Arc<Shadow>, Vec<(usize, usize)>)>, String> {
        if t.len() % 3 != 0 {
            return Err("ranges come as <shadow> <off> <len>".into());
        }
        let mut out: Vec<(String, (Arc<Shadow>, Vec<(usize, usize)>))> = Vec::new();
        for r in t.chunks(3) {
            let c = self.copy(r[0])?;
            let at = (c.shadow.start() as i64 + num(r[1])?) as usize;
            let range = (at, at + unum(r[2])?);
            match out.iter_mut().find(|(n, _)| n == r[0]) {
                Some((_, d)) => d.1.push(range),
                None => out.push((r[0].to_string(), (c.shadow.clone(), vec![range]))),
            }
        }
        Ok(out.into_iter().map(|(_, d)| d).collect())
    }

              fn log_card(&mut self, name: &str) -> Result<(), String> {
        let c = self.copy(name)?;
        let Some((buffer, before, size)) = c.shadow.card_parts() else {
            return Ok(());
        };
        let host = crate::host::host().ok_or("no D3D12 device")?;
        let vram = fnv64(&host.read_back(&buffer, size));
        let hashes = match before {
            Some(b) => format!("{:016x}", fnv64(&host.read_back(&b, crate::shadow::card_hash_bytes(size)))),
            None => "none".to_string(),
        };
        writeln!(self.log, "  card {name} vram={vram:016x} hashes={hashes}").unwrap();
        Ok(())
    }

              fn log_card_stats(&mut self) {
        let s = crate::shadow::card_stats();
        writeln!(
            self.log,
            "  card stats sent={} ranges={} compared={} fetched={} fetched_bytes={} waits={} copies={} bytes={} hash_bytes={}",
            s[0], s[1], s[2], s[3], s[4], s[5], s[7], s[8], s[9]
        )
        .unwrap();
    }

    fn log_writes(&mut self) {
        for (at, len) in crate::host::take_writes() {
            writeln!(self.log, "  title write {at:#x} {len:#x}").unwrap();
        }
    }

         fn ranges(&self, t: &[&str]) -> Result<Vec<(usize, usize)>, String> {
        if t.len() % 3 != 0 {
            return Err("ranges come as <shadow> <off> <len>".into());
        }
        t.chunks(3)
            .map(|r| {
                let c = self.copy(r[0])?;
                Ok(((c.shadow.start() as i64 + num(r[1])?) as usize, unum(r[2])?))
            })
            .collect()
    }

    fn log_copies(&mut self) {
        for (at, len) in crate::host::take_copies() {
            writeln!(self.log, "  copy {at:#x} {len:#x}").unwrap();
        }
    }

                   fn log_state(&mut self, name: &str, from: usize, to: usize) -> Result<(), String> {
        self.log_state_snap(name, from, to, false)
    }

                   fn log_state_snap(&mut self, name: &str, from: usize, to: usize, snap: bool) -> Result<(), String> {
        let c = self.copy(name)?;
        let s = &c.shadow;
        let (start, len) = (s.start() as usize, s.len() as usize);

        let bytes = unsafe { std::slice::from_raw_parts(s.data(), len) };
        let (mode, at, uncopied, pending) = s.state();
        let (from, to) = (from.max(start), to.min(start + len));
        let mut drift = 0usize;
        if from < to {
            let title = read_title(from, to - from);
            for (k, &t) in title.iter().enumerate() {
                let a = from + k;
                if pending.iter().any(|&(p, e)| p <= a && a < e) {
                    continue;
                }
                if bytes[a - start] != t {
                    drift += 1;
                }
            }
        }
        let mut pages = String::new();
        let mut i = 0;
        while i < at.len() {
            let mut j = i;
            while j < at.len() && at[j] == at[i] {
                j += 1;
            }
            if !pages.is_empty() {
                pages.push(',');
            }
            write!(pages, "{}x{}", at[i], j - i).unwrap();
            i = j;
        }
        let pend: Vec<String> = pending.iter().map(|(a, e)| format!("{a:#x}-{e:#x}")).collect();
        let hash = fnv64(bytes);
        let whole_title = read_title(start, len);
        let title_hash = fnv64(&whole_title);


        let mut stale = 0usize;
        for (k, &t) in whole_title.iter().enumerate() {
            let a = start + k;
            if bytes[k] != t && !pending.iter().any(|&(p, e)| p <= a && a < e) {
                stale += 1;
            }
        }
        let snap_text = if snap {
            let offsets: Vec<(usize, usize)> = pending
                .iter()
                .map(|&(a, e)| (a.clamp(start, start + len) - start, e.clamp(start, start + len) - start))
                .collect();
            match s.snapshot_hash(&offsets) {
                Some(h) => format!(" snap={h:016x}"),
                None => " snap=none".to_string(),
            }
        } else {
            String::new()
        };
        writeln!(
            self.log,
            "  state {name} mode={mode} uncopied={uncopied} pages={pages} pending=[{}] hash={hash:016x} title={title_hash:016x} drift={drift} stale={stale}{snap_text}",
            pend.join(",")
        )
        .unwrap();
        Ok(())
    }

    fn op(&mut self, t: &[&str], out: &Path) -> Result<(), String> {
        let need = |n: usize| if t.len() == n { Ok(()) } else { Err(format!("{} takes {} arguments", t[0], n - 1)) };
        match t[0] {
            "env" => {}  
            "quiet" => {
                need(2)?;
                self.quiet = unum(t[1])? != 0;
            }
            "region" => {
                need(5)?;
                let (addr, bytes) = (unum(t[3])?, unum(t[4])?);
                let flags = match t[2] {
                    "watched" => MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                    "reserve" => MEM_RESERVE | MEM_WRITE_WATCH,
                    "plain" => MEM_RESERVE | MEM_COMMIT,
                    k => return Err(format!("unknown region kind {k}")),
                };

                let p = unsafe { VirtualAlloc(addr as *const _, bytes, flags, PAGE_READWRITE) } as usize;
                if p != addr {
                    return Err(format!("region {} not at {addr:#x} (got {p:#x})", t[1]));
                }
                self.places.insert(t[1].into(), addr);
                self.place_len.insert(t[1].into(), bytes);
            }
            "section" => {
                need(3)?;
                let bytes = unum(t[2])? as u64;

                let h = unsafe {
                    CreateFileMappingW(INVALID_HANDLE_VALUE, std::ptr::null(), PAGE_READWRITE, (bytes >> 32) as u32, bytes as u32, std::ptr::null())
                };
                if h.is_null() {
                    return Err(format!("section {} failed", t[1]));
                }
                self.sections.insert(t[1].into(), h);
            }
            "view" => {
                need(6)?;
                let (h, offset, addr, bytes) = (self.section(t[2])?, unum(t[3])? as u64, unum(t[4])?, unum(t[5])?);

                let v = unsafe { MapViewOfFileEx(h, FILE_MAP_WRITE, (offset >> 32) as u32, offset as u32, bytes, addr as *const _) }.Value as usize;
                if v != addr {
                    return Err(format!("view {} not at {addr:#x} (got {v:#x})", t[1]));
                }
                crate::watch::title_memory_remapped(addr, bytes);
                crate::inplace::title_memory_view(addr, bytes, h as usize, offset);
                self.places.insert(t[1].into(), addr);
                self.place_len.insert(t[1].into(), bytes);
            }
            "placeholder" => {
                need(4)?;
                let (addr, blocks) = (unum(t[2])?, unum(t[3])?);

                let p = unsafe {
                    VirtualAlloc2(GetCurrentProcess(), addr as *const _, blocks * BLOCK, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, std::ptr::null_mut(), 0)
                } as usize;
                if p != addr {
                    return Err(format!("placeholder {} not at {addr:#x} (got {p:#x})", t[1]));
                }
                self.places.insert(t[1].into(), addr);
                self.place_len.insert(t[1].into(), blocks * BLOCK);
            }
            "map" => {
                need(5)?;
                let at = self.place(t[1])? + unum(t[2])? * BLOCK;
                let (h, offset) = (self.section(t[3])?, unum(t[4])? as u64);


                let v = unsafe {
                    VirtualFree(at as *mut _, BLOCK, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
                    MapViewOfFile3(h, GetCurrentProcess(), at as *const _, offset, BLOCK, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, std::ptr::null_mut(), 0)
                }
                .Value as usize;
                if v != at {
                    return Err(format!("map at {at:#x} failed"));
                }
                crate::watch::title_memory_remapped(at, BLOCK);
                crate::inplace::title_memory_view(at, BLOCK, h as usize, offset);
            }
            "unmap" => {
                need(3)?;
                let at = self.place(t[1])? + unum(t[2])? * BLOCK;
                crate::watch::title_memory_remapped(at, BLOCK);

                let ok = unsafe { UnmapViewOfFile2(GetCurrentProcess(), MEMORY_MAPPED_VIEW_ADDRESS { Value: at as *mut _ }, MEM_PRESERVE_PLACEHOLDER) };
                if ok == 0 {
                    return Err(format!("unmap at {at:#x} failed"));
                }
            }
            "commit" | "decommit" => {
                need(4)?;
                let (at, len) = (self.place(t[1])? + unum(t[2])?, unum(t[3])?);

                let ok = unsafe {
                    if t[0] == "commit" {
                        !VirtualAlloc(at as *const _, len, MEM_COMMIT, PAGE_READWRITE).is_null()
                    } else {
                        VirtualFree(at as *mut _, len, MEM_DECOMMIT) != 0
                    }
                };
                writeln!(self.log, "  ok {}", u8::from(ok)).unwrap();
                crate::watch::title_memory_remapped(at, len);
            }
            "remapped" => {
                need(4)?;
                crate::watch::title_memory_remapped(self.place(t[1])? + unum(t[2])?, unum(t[3])?);
            }
            "write" | "kwrite" => {
                need(5)?;
                let (at, len, seed) = (self.place(t[1])? + unum(t[2])?, unum(t[3])?, unum(t[4])? as u32);
                if t[0] == "kwrite" {
                    crate::watch::writing(at, len);
                }
                let (w, s) = title_write(at, len, seed);
                writeln!(self.log, "  wrote {w} skipped {s}").unwrap();
            }
            "wrote" => {
                need(4)?;
                crate::watch::wrote(self.place(t[1])? + unum(t[2])?, unum(t[3])?);
            }
            "shadow" => {
                need(5)?;
                let (start, len) = (self.place(t[2])? + unum(t[3])?, unum(t[4])?);

                let data = unsafe { VirtualAlloc(std::ptr::null(), len.max(1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) } as *mut u8;
                if data.is_null() {
                    return Err("host buffer".into());
                }
                let shadow = Arc::new(Shadow::for_test(data, start as u64, len as u64));
                self.copies.push(Copy { name: t[1].into(), shadow });
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state(&name, 0, 0)?;
                }
            }
            "card_shadow" => {

                need(5)?;
                crate::host::host().ok_or("no D3D12 device")?;
                let (start, len) = (self.place(t[2])? + unum(t[3])?, unum(t[4])?);
                self.next_resource += 1;
                crate::shadow::register(self.next_resource, start as u64, len as u64);
                self.buffers.insert(t[1].into(), (self.next_resource, start as u64));
                let shadow = crate::shadow::find(start as u64, len as u64).ok_or("no copy")?;
                writeln!(self.log, "  carded {}", u8::from(shadow.carded())).unwrap();
                self.copies.retain(|c| c.name != t[1]);
                self.copies.push(Copy { name: t[1].into(), shadow });
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state(&name, 0, 0)?;
                }
            }
            "card_write" => {
                need(5)?;
                let c = self.copy(t[1])?;
                let (off, len, seed) = (unum(t[2])?, unum(t[3])?, unum(t[4])? as u32);
                let total = c.shadow.len() as usize;
                let (from, to) = (off.min(total), off.saturating_add(len).min(total));
                let bytes: Vec<u8> = (from..to).map(|k| pattern(seed, k - off)).collect();
                if c.shadow.carded() {
                    let shadow = c.shadow.clone();
                    self.card_writes.push((shadow, from as u64, bytes));
                } else {


                    for (k, b) in bytes.iter().enumerate() {

                        unsafe { c.shadow.data().add(from + k).write(*b) };
                    }
                }
                writeln!(self.log, "  gpu wrote {}", to - from).unwrap();
            }
            "card_submit" => {


                let ranges = self.ranges(&t[1..])?;
                let names: Vec<&str> = t[1..].chunks(3).map(|r| r[0]).collect();
                let mut writes = Vec::new();
                for (name, &(at, len)) in names.iter().zip(&ranges) {
                    writes.push((self.copy(name)?.shadow.clone(), at, len));
                }
                let back = self.declared(&t[1..])?;
                let host = crate::host::host().ok_or("no D3D12 device")?;
                let staged: Vec<_> = std::mem::take(&mut self.card_writes)
                    .into_iter()
                    .map(|(s, off, bytes)| (s.buffer.clone(), off, bytes))
                    .collect();
                let (title, _source) = host.title_list(&staged);
                let list = crate::host::HostList::writing(writes);
                let ns = host.submit_cards(&[&list], &title.1, &back);
                writeln!(self.timing, "{} card_before {}", self.op_number, ns[0]).unwrap();
                writeln!(self.timing, "{} card_gpu {}", self.op_number, ns[1]).unwrap();
                writeln!(self.timing, "{} card_back {}", self.op_number, ns[2]).unwrap();
                writeln!(self.timing, "{} card_submit {}", self.op_number, ns[0] + ns[1] + ns[2]).unwrap();


                let mut writes = crate::host::take_writes();
                writes.sort_unstable();
                for (at, len) in writes {
                    writeln!(self.log, "  title write {at:#x} {len:#x}").unwrap();
                }
                self.log_card_stats();
                let mut seen: Vec<&str> = Vec::new();
                for n in names {
                    if !seen.contains(&n) {
                        seen.push(n);
                    }
                }
                if !self.quiet {
                    for name in seen {
                        let name = name.to_string();
                        self.log_state_snap(&name, 0, usize::MAX, true)?;
                        self.log_card(&name)?;
                    }
                }
            }
            "card_send" => {

                let ranges = self.ranges(&t[1..])?;
                let names: Vec<&str> = t[1..].chunks(3).map(|r| r[0]).collect();
                let mut writes = Vec::new();
                for (name, &(at, len)) in names.iter().zip(&ranges) {
                    writes.push((self.copy(name)?.shadow.clone(), at, len));
                }
                let back = self.declared(&t[1..])?;
                let host = crate::host::host().ok_or("no D3D12 device")?;

                if crate::host::relay() && host.results_low() {
                    let flights: Vec<_> = self.flights.drain(..).collect();
                    let ns = host.land_cards(flights);
                    writeln!(self.timing, "{} card_land {}", self.op_number, ns[1]).unwrap();
                    writeln!(self.log, "  landed all first").unwrap();
                }
                let staged: Vec<_> = std::mem::take(&mut self.card_writes)
                    .into_iter()
                    .map(|(s, off, bytes)| (s.buffer.clone(), off, bytes))
                    .collect();
                let title = host.title_list(&staged);
                let list = crate::host::HostList::writing(writes);
                let (flight, ns) = host.send_cards(&[&list], title, back);
                self.flights.push_back(flight);
                writeln!(self.timing, "{} card_before {}", self.op_number, ns[0]).unwrap();
                writeln!(self.timing, "{} card_send {}", self.op_number, ns[0] + ns[1]).unwrap();
                writeln!(self.log, "  in the air {}", self.flights.len()).unwrap();
            }
            "card_land" => {

                let n = if t.len() > 1 { unum(t[1])?.min(self.flights.len()) } else { self.flights.len() };
                let flights: Vec<_> = self.flights.drain(..n).collect();
                let mut names: Vec<String> = Vec::new();
                for f in &flights {
                    for c in &self.copies {
                        if f.names(&c.shadow) && !names.contains(&c.name) {
                            names.push(c.name.clone());
                        }
                    }
                }
                let host = crate::host::host().ok_or("no D3D12 device")?;
                let ns = host.land_cards(flights);
                writeln!(self.timing, "{} card_wait {}", self.op_number, ns[0]).unwrap();
                writeln!(self.timing, "{} card_land {}", self.op_number, ns[1]).unwrap();
                let mut writes = crate::host::take_writes();
                writes.sort_unstable();
                for (at, len) in writes {
                    writeln!(self.log, "  title write {at:#x} {len:#x}").unwrap();
                }
                self.log_card_stats();
                if !self.quiet {
                    for name in names {
                        self.log_state_snap(&name, 0, usize::MAX, true)?;
                        self.log_card(&name)?;
                    }
                }
            }

            "rebar_level" => {
                need(2)?;
                let v = if t[1] == "-" { None } else { Some(t[1]) };
                writeln!(self.log, "  level {}", crate::rebar::level_of(v)).unwrap();
            }
            "rebar_most" => {
                need(2)?;
                let b = if t[1] == "-" { None } else { Some(unum(t[1])? as u64) };
                writeln!(self.log, "  most {}", crate::rebar::default_most(b)).unwrap();
            }
            "rebar_decide" => {
                need(10)?;
                let ask = crate::rebar::Ask {
                    level: unum(t[1])? as u8,
                    supported: t[2] == "1",
                    kind: if t[3] == "window" { crate::rebar::Kind::Window } else { crate::rebar::Kind::Buffer },
                    size: unum(t[4])? as u64,
                    cpu_read: t[5] == "1",
                    placed: unum(t[6])? as u64,
                    most: unum(t[7])? as u64,
                    card: if t[8] == "-" { None } else { Some((unum(t[8])? as u64, unum(t[9])? as u64)) },
                };
                writeln!(self.log, "  decision {:?}", crate::rebar::decide(&ask)).unwrap();
            }
            "ranges_insert" => {
                need(3)?;
                self.range_set.insert(unum(t[1])? as u64, unum(t[2])? as u64);
            }
            "ranges_remove" => {
                need(3)?;
                let r = self.range_set.remove(unum(t[1])? as u64, unum(t[2])? as u64);
                writeln!(self.log, "  removed {}", u8::from(r)).unwrap();
            }
            "ranges_remove_at" => {
                need(2)?;
                let r = self.range_set.remove_at(unum(t[1])? as u64);
                writeln!(self.log, "  removed {}", u8::from(r)).unwrap();
            }
            "ranges_overlaps" => {
                need(3)?;
                let started = std::time::Instant::now();
                let r = self.range_set.overlaps(unum(t[1])? as u64, unum(t[2])? as u64);
                writeln!(self.timing, "{} ranges_overlaps {}", self.op_number, started.elapsed().as_nanos()).unwrap();
                writeln!(self.log, "  overlaps {} len {}", u8::from(r), self.range_set.len()).unwrap();
            }
            "lane_hash" => {
                need(4)?;
                let (at, len) = (self.place(t[1])? + unum(t[2])?, unum(t[3])?);

                let bytes = unsafe { std::slice::from_raw_parts(at as *const u8, len) };
                let started = std::time::Instant::now();
                let h = crate::memcopy::lane_hash(bytes);
                writeln!(self.timing, "{} lane_hash {}", self.op_number, started.elapsed().as_nanos()).unwrap();
                let hex: String = h.iter().map(|b| format!("{b:02x}")).collect();
                writeln!(self.log, "  hash {hex}").unwrap();
            }
            "check_turn" => {
                need(4)?;
                let r = crate::memcopy::check_turn(unum(t[1])? as u64, unum(t[2])?, unum(t[3])? as u64);
                writeln!(self.log, "  turn {}", u8::from(r)).unwrap();
            }
            "hash_every" => {
                need(2)?;
                let v = if t[1] == "-" { None } else { Some(t[1]) };
                writeln!(self.log, "  every {}", crate::memcopy::hash_every_for_test(v)).unwrap();
            }

            "buffer" => {
                need(5)?;
                let (start, len) = (self.place(t[2])? + unum(t[3])?, unum(t[4])?);
                self.next_resource += 1;
                crate::shadow::register(self.next_resource, start as u64, len as u64);
                self.buffers.insert(t[1].into(), (self.next_resource, start as u64));
            }
            "unbuffer" => {
                need(2)?;
                let (res, start) = self.buffers.remove(t[1]).ok_or("unknown buffer")?;
                crate::shadow::unregister(res, start);
            }
            "find" | "find_any" => {
                need(5)?;


                crate::host::host().ok_or("no D3D12 device")?;
                let (at, len) = (self.place(t[2])? + unum(t[3])?, unum(t[4])?);
                let started = std::time::Instant::now();
                let shadow = if t[0] == "find" { crate::shadow::find(at as u64, len as u64) } else { crate::shadow::find_any(at as u64, len as u64) };
                writeln!(self.timing, "{} {} {}", self.op_number, t[0], started.elapsed().as_nanos()).unwrap();
                let shadow = shadow.ok_or("no copy")?;
                writeln!(
                    self.log,
                    "  found {:#x}+{:#x} window={} carded={} rebar={} placed={}",
                    shadow.start(),
                    shadow.len(),
                    u8::from(shadow.is_window()),
                    u8::from(shadow.carded()),
                    u8::from(shadow.rebar_bytes() > 0),
                    u8::from(shadow.is_placed())
                )
                .unwrap();
                self.copies.retain(|c| c.name != t[1]);
                self.copies.push(Copy { name: t[1].into(), shadow });
            }
            "card_check" => {
                need(2)?;
                let name = t[1].to_string();
                self.log_card(&name)?;
            }
            "refresh" => {
                need(4)?;
                let c = self.copy(t[1])?;
                let (at, len) = ((c.shadow.start() as i64 + num(t[2])?) as usize, unum(t[3])?);
                let started = std::time::Instant::now();
                let n = c.shadow.update(at, len);
                let ns = started.elapsed().as_nanos();
                writeln!(self.timing, "{} refresh {ns}", self.op_number).unwrap();
                self.log_copies();
                writeln!(self.log, "  ret {n}").unwrap();
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state(&name, at, at.saturating_add(len))?;
                }
            }
            "flush" => {

                let ranges = self.ranges(&t[1..])?;
                let names: Vec<&str> = t[1..].chunks(3).map(|r| r[0]).collect();
                let ahead = ranges.len() > 1;

                let shadows: Vec<&Shadow> = names.iter().map(|n| self.copy(n).map(|c| &*c.shadow)).collect::<Result<_, _>>()?;
                let started = std::time::Instant::now();
                let mut rets = Vec::new();
                if ahead {
                    let asked: Vec<(usize, usize)> = shadows
                        .iter()
                        .zip(&ranges)
                        .filter(|(s, _)| !s.unwatched() && !s.is_view_guarded())
                        .map(|(_, &(a, l))| (a, a + l))
                        .collect();
                    crate::watch::ask_ahead(&asked);
                }
                for (s, &(at, len)) in shadows.iter().zip(&ranges) {
                    let n = s.update(at, len);
                    rets.push((crate::host::take_copies(), n));
                }
                if ahead {
                    crate::watch::ask_ahead_done();
                }
                let ns = started.elapsed().as_nanos();
                writeln!(self.timing, "{} flush {ns}", self.op_number).unwrap();
                for (name, (copies, n)) in names.iter().zip(rets) {
                    for (at, len) in copies {
                        writeln!(self.log, "  copy {at:#x} {len:#x}").unwrap();
                    }
                    writeln!(self.log, "  ret {name} {n}").unwrap();
                }
                if !self.quiet {
                    for (name, &(at, len)) in names.iter().zip(&ranges) {
                        self.log_state(name, at, at + len)?;
                    }
                }
            }
            "stale" => {
                need(4)?;
                let c = self.copy(t[1])?;
                let (at, len) = ((c.shadow.start() as i64 + num(t[2])?) as usize, unum(t[3])?);
                let started = std::time::Instant::now();
                let stale = c.shadow.stale_under_writes(at, len);
                writeln!(self.timing, "{} stale {}", self.op_number, started.elapsed().as_nanos()).unwrap();
                writeln!(self.log, "  stale {}", u8::from(stale)).unwrap();
            }
            "prepare" => {
                need(2)?;
                crate::shadow::preparing(unum(t[1])? as u64);
            }
            "gpu_declare" => {
                need(4)?;
                let c = self.copy(t[1])?;
                let (at, len) = ((c.shadow.start() as i64 + num(t[2])?) as usize, unum(t[3])?);
                c.shadow.writing(at, len);
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state(&name, 0, 0)?;
                }
            }
            "gpu_write" => {
                need(5)?;
                let c = self.copy(t[1])?;
                let (off, len, seed) = (unum(t[2])?, unum(t[3])?, unum(t[4])? as u32);
                let total = c.shadow.len() as usize;
                let (from, to) = (off.min(total), off.saturating_add(len).min(total));
                for k in from..to {

                    unsafe { c.shadow.data().add(k).write(pattern(seed, k - off)) };
                }
                writeln!(self.log, "  gpu wrote {}", to - from).unwrap();
            }
            "gpu_done" => {
                if t.len() < 4 || (t.len() - 2) % 2 != 0 {
                    return Err("gpu_done <shadow> <off> <len> [<off> <len>...]".into());
                }
                let c = self.copy(t[1])?;
                let start = c.shadow.start() as i64;
                let ranges: Vec<(usize, usize)> = t[2..]
                    .chunks(2)
                    .map(|r| {
                        let at = (start + num(r[0])?) as usize;
                        Ok((at, at + unum(r[1])?))
                    })
                    .collect::<Result<_, String>>()?;
                c.shadow.copy_back_done(&ranges);
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state(&name, 0, 0)?;
                }
            }
            "ask_ahead" | "ask_once" => {
                let ranges: Vec<(usize, usize)> = self.ranges(&t[1..])?.into_iter().map(|(a, l)| (a, a + l)).collect();
                if t[0] == "ask_ahead" {
                    crate::watch::ask_ahead(&ranges);
                } else {
                    crate::watch::ask_once(&ranges);
                }
            }
            "snapshots" => {
                let started = std::time::Instant::now();
                crate::shadow::take_snapshots();
                writeln!(self.timing, "{} snapshots {}", self.op_number, started.elapsed().as_nanos()).unwrap();
            }
            "hand_back" | "relay_back" => {
                let back = self.declared(&t[1..])?;
                let started = std::time::Instant::now();
                crate::host::land(&back, t[0] == "relay_back");
                writeln!(self.timing, "{} {} {}", self.op_number, t[0], started.elapsed().as_nanos()).unwrap();
                self.log_writes();
                let mut names: Vec<&str> = Vec::new();
                for r in t[1..].chunks(3) {
                    if !names.contains(&r[0]) {
                        names.push(r[0]);
                    }
                }
                if !self.quiet {
                    for name in names {
                        let name = name.to_string();
                        self.log_state_snap(&name, 0, usize::MAX, true)?;
                    }
                }
            }
            "copy_back" => {
                need(4)?;
                let c = self.copy(t[1])?;
                let (at, len) = ((c.shadow.start() as i64 + num(t[2])?) as usize, unum(t[3])?);
                let started = std::time::Instant::now();
                let n = c.shadow.copy_back(at, len);
                writeln!(self.timing, "{} copy_back {}", self.op_number, started.elapsed().as_nanos()).unwrap();
                self.log_writes();
                writeln!(self.log, "  back {n}").unwrap();
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state_snap(&name, 0, usize::MAX, true)?;
                }
            }
            "merge_under" => {
                need(5)?;
                let c = self.copy(t[1])?;
                let (at, len) = ((c.shadow.start() as i64 + num(t[2])?) as usize, unum(t[3])?);
                let mut runs_left = unum(t[4])?;
                let started = std::time::Instant::now();
                let settle = c.shadow.merge_under_writes(at, len, &mut runs_left);
                writeln!(self.timing, "{} merge_under {}", self.op_number, started.elapsed().as_nanos()).unwrap();
                self.log_copies();
                writeln!(self.log, "  settle {} runs_left {runs_left}", u8::from(settle)).unwrap();
                let name = t[1].to_string();
                if !self.quiet {
                    self.log_state_snap(&name, 0, usize::MAX, true)?;
                }
            }
            "ask_ahead_done" => crate::watch::ask_ahead_done(),
            "ask_once_end" => crate::watch::ask_once_end(),
            "check" => {
                let names: Vec<String> = self.copies.iter().map(|c| c.name.clone()).collect();
                for name in names {
                    self.log_state_snap(&name, 0, usize::MAX, true)?;
                }
            }
            "end" => {
                let names: Vec<String> = self.copies.iter().map(|c| c.name.clone()).collect();
                for name in &names {
                    self.log_state_snap(name, 0, usize::MAX, true)?;
                    let c = self.copy(name)?;

                    let bytes = unsafe { std::slice::from_raw_parts(c.shadow.data(), c.shadow.len() as usize) };
                    std::fs::write(out.join(format!("shadow-{name}.bin")), bytes).map_err(|e| e.to_string())?;
                }

                if !self.quiet {
                    let places: Vec<(String, usize)> = self.places.iter().map(|(n, &a)| (n.clone(), a)).collect();
                    for (name, at) in places {
                        let len = self.place_len.get(&name).copied().unwrap_or(0);
                        std::fs::write(out.join(format!("title-{name}.bin")), read_title(at, len)).map_err(|e| e.to_string())?;
                    }
                }
            }
            other => return Err(format!("unknown operation {other}")),
        }
        Ok(())
    }
}

pub fn run(scenario: &Path, out: &Path) -> Result<(), String> {
    let text = std::fs::read_to_string(scenario).map_err(|e| format!("{}: {e}", scenario.display()))?;
    std::fs::create_dir_all(out).map_err(|e| e.to_string())?;
    let lines: Vec<Vec<&str>> = text
        .lines()
        .map(|l| l.split('#').next().unwrap_or("").split_whitespace().collect::<Vec<_>>())
        .filter(|t| !t.is_empty())
        .collect();

    for t in &lines {
        if t[0] == "env" {
            if t.len() != 3 {
                return Err("env <NAME> <VALUE>".into());
            }

            unsafe { std::env::set_var(t[1], t[2]) };
        }
    }
    let mut world = World::default();
    let mut result = Ok(());
    for (n, t) in lines.iter().enumerate() {
        writeln!(world.log, "#{n} {}", t.join(" ")).unwrap();
        world.op_number = n;
        if let Err(e) = world.op(t, out) {
            writeln!(world.log, "  error {e}").unwrap();
            result = Err(format!("operation {n} ({}): {e}", t.join(" ")));
            break;
        }
    }
    std::fs::write(out.join("log.txt"), &world.log).map_err(|e| e.to_string())?;
    std::fs::write(out.join("timing.txt"), &world.timing).map_err(|e| e.to_string())?;
    result
}
