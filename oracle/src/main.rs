
#![allow(dead_code, unused_imports, unused_variables, unused_mut, unused_unsafe)]
#![allow(clippy::all)]

pub mod watch {
    include!(concat!(env!("OUT_DIR"), "/watch.rs"));
}

pub mod inplace {
    use std::collections::BTreeMap;
    use std::ffi::c_void;
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicU64, Ordering};

    use windows::Win32::Foundation::HANDLE;
    use windows::Win32::Graphics::Direct3D12::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC;
    use windows::core::Interface;
    use windows_sys::Win32::System::Memory as mem;

    include!(concat!(env!("OUT_DIR"), "/inplace_reference.rs"));
}

pub mod descriptor {
    pub mod host {
                          pub(crate) fn remapped(_at: usize, _len: usize) {}
    }
}

pub mod texture {
         pub(crate) struct Staging;

              pub(crate) fn committed(_at: *const u8, _len: usize) -> bool {
        unreachable!("copies into title memory are not part of milestone 1")
    }
}

pub mod host {
    use std::cell::RefCell;
    use std::sync::OnceLock;
    use std::sync::atomic::Ordering;
    include!(concat!(env!("OUT_DIR"), "/host_reference.rs"));

    thread_local! {
                 pub static WRITES: RefCell<Vec<(usize, usize)>> = const { RefCell::new(Vec::new()) };
    }

    pub(crate) fn write_mapped(dst: *mut u8, src: *const u8, len: usize) {
        WRITES.with(|c| c.borrow_mut().push((dst as usize, len)));
        write_mapped_reference(dst, src, len)
    }

    pub fn take_writes() -> Vec<(usize, usize)> {
        WRITES.with(|c| std::mem::take(&mut *c.borrow_mut()))
    }

         pub(crate) fn land(back: &[Declared], relayed: bool) {
        if relayed { relay_hand_back(back) } else { hand_back(back) }
    }

    thread_local! {
                 pub static COPIES: RefCell<Vec<(usize, usize)>> = const { RefCell::new(Vec::new()) };
    }

    pub(crate) fn copy_mapped(dst: *mut u8, src: *const u8, len: usize, title_dst: bool) {
        COPIES.with(|c| c.borrow_mut().push((src as usize, len)));
        copy_mapped_reference(dst, src, len, title_dst)
    }

    pub fn take_copies() -> Vec<(usize, usize)> {
        COPIES.with(|c| std::mem::take(&mut *c.borrow_mut()))
    }

    pub(crate) use card::*;

    pub mod card {
    use std::sync::atomic::Ordering;
    use std::sync::{Mutex, OnceLock};

    use windows::Win32::Foundation::HANDLE;
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_12_0;
    use windows::Win32::Graphics::Direct3D12::*;
    use windows::Win32::Graphics::Dxgi::Common::*;
    use windows::Win32::Graphics::Dxgi::*;
    use windows::Win32::System::Threading::{CreateEventW, INFINITE, WaitForSingleObject};
    use windows::core::Interface;

    use super::{COPY_BACK, Declared, hand_back, relay_hand_back};

    include!(concat!(env!("OUT_DIR"), "/host_card_reference.rs"));


    unsafe impl Send for CardWork {}

         pub(crate) struct Host {
        pub(crate) device: ID3D12Device,
        queue: ID3D12CommandQueue,
        fence: ID3D12Fence,
        event: HANDLE,
        adapter: Option<IDXGIAdapter3>,
                          submitted: Mutex<u64>,
                          relay_fetch: Mutex<Option<RelayFetch>>,
    }

                        pub(crate) struct Flight {
        card: Option<CardPlan>,
        back: Vec<Declared>,
        fence: u64,
        _title: (Prelude, Option<ID3D12Resource>),
    }

    impl Flight {
                 pub(crate) fn names(&self, shadow: &std::sync::Arc<crate::shadow::Shadow>) -> bool {
            self.back.iter().any(|(s, _)| std::sync::Arc::ptr_eq(s, shadow))
        }
    }


    unsafe impl Send for Host {}
    unsafe impl Sync for Host {}

    static HOST: OnceLock<Option<Host>> = OnceLock::new();

    pub(crate) fn host() -> Option<&'static Host> {
        HOST.get_or_init(make_host).as_ref()
    }

              fn make_host() -> Option<Host> {

        unsafe {
            let factory: IDXGIFactory6 = CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0)).ok()?;
            let mut device: Option<ID3D12Device> = None;
            let mut chosen: Option<IDXGIAdapter1> = None;
            if std::env::var("DANZI_TEST_ADAPTER").is_ok_and(|v| v == "warp") {
                let a: IDXGIAdapter1 = factory.EnumWarpAdapter().ok()?;
                D3D12CreateDevice(&a, D3D_FEATURE_LEVEL_12_0, &mut device).ok()?;
                chosen = Some(a);
            } else {
                for i in 0.. {
                    let Ok(a) = factory.EnumAdapterByGpuPreference::<IDXGIAdapter1>(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE) else {
                        break;
                    };
                    let Ok(desc) = a.GetDesc1() else { continue };
                    if desc.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32) != 0 {
                        continue;
                    }
                    if D3D12CreateDevice(&a, D3D_FEATURE_LEVEL_12_0, &mut device).is_ok() {
                        chosen = Some(a);
                        break;
                    }
                }
            }
            let device = device?;

            let budget = chosen.as_ref().and_then(|a| a.cast::<IDXGIAdapter3>().ok()).and_then(|a| {
                let mut own = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
                a.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut own).ok().map(|()| own.Budget)
            });
            crate::rebar::probe(&device, budget);
            let queue: ID3D12CommandQueue = device
                .CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC {
                    Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
                    ..Default::default()
                })
                .ok()?;
            let fence: ID3D12Fence = device.CreateFence(0, D3D12_FENCE_FLAG_NONE).ok()?;
            let event = CreateEventW(None, false, false, None).ok()?;
            Some(Host {
                device,
                queue,
                fence,
                event,
                adapter: chosen.and_then(|a| a.cast().ok()),
                submitted: Mutex::new(0),
                relay_fetch: Mutex::new(None),
            })
        }
    }

              pub(crate) fn retire<T: 'static>(object: T) {
        drop(object);
    }

              pub(crate) fn moment() -> u64 {
        0
    }

              pub(crate) fn name(_object: &impl Interface, _text: impl FnOnce() -> String) {}

              pub(crate) fn fail(what: std::fmt::Arguments) -> ! {
        panic!("host GPU failure: {what}")
    }

              fn working(_on: bool) -> bool {
        false
    }

    fn stood(_since: std::time::Instant) {}

              pub(crate) struct Deferred {
        copies: Vec<Step>,
    }

    pub(crate) struct HostList {
        deferred: Mutex<Deferred>,
    }

    impl HostList {
                 pub(crate) fn writing(writes: Vec<(std::sync::Arc<crate::shadow::Shadow>, usize, usize)>) -> HostList {
            HostList {
                deferred: Mutex::new(Deferred {
                    copies: writes.into_iter().map(|(s, a, l)| Step::GpuWrite(s, a, l)).collect(),
                }),
            }
        }

        pub(crate) fn deferred(&self) -> std::sync::MutexGuard<'_, Deferred> {
            self.deferred.lock().unwrap_or_else(|e| e.into_inner())
        }
    }

    impl Host {
                 fn card_sent(&self) {}

                          pub(crate) fn video_memory(&self) -> Option<[u64; 4]> {
            let adapter = self.adapter.as_ref()?;
            let mut own = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
            let mut shared = DXGI_QUERY_VIDEO_MEMORY_INFO::default();

            unsafe {
                adapter.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut own).ok()?;
                adapter.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mut shared).ok()?;
            }


            let budget = match std::env::var("VICTORY_TEST_VRAM_BUDGET_MB").ok().and_then(|v| v.trim().parse::<u64>().ok()) {
                Some(mb) => own.Budget.min(mb << 20),
                None => own.Budget,
            };
            Some([budget, own.CurrentUsage, shared.Budget, shared.CurrentUsage])
        }

                                                              pub(crate) fn submit_cards(&self, runs: &[&HostList], title: &ID3D12GraphicsCommandList, back: &[Declared]) -> [u128; 3] {
            let mut submitted = self.submitted.lock().unwrap_or_else(|e| e.into_inner());
            let started = std::time::Instant::now();
            let card = self.card_before(runs, false);
            let mut lists: Vec<Option<ID3D12CommandList>> = vec![Some(title.cast().unwrap())];
            if let Some(check) = card.as_ref().and_then(|c| c.check.clone()) {
                lists.push(Some(check));
            }
            let prepared = std::time::Instant::now();
            *submitted += 1;

            unsafe {
                self.queue.ExecuteCommandLists(&lists);
                self.card_sent();
                self.queue.Signal(&self.fence, *submitted).unwrap();
                self.fence.SetEventOnCompletion(*submitted, self.event).unwrap();
                WaitForSingleObject(self.event, INFINITE);
            }
            let ran = std::time::Instant::now();
            if let Some(card) = &card {
                self.card_back(card, &mut submitted, &mut std::collections::HashSet::new());
            }
            hand_back(back);
            if let Some(card) = card {
                self.card_done(card);
            }
            if relay() {
                crate::shadow::relay_fetched(*QUEUE_ORDER.lock().unwrap_or_else(|e| e.into_inner()));
            }
            let done = std::time::Instant::now();
            [(prepared - started).as_nanos(), (ran - prepared).as_nanos(), (done - ran).as_nanos()]
        }

                                                              pub(crate) fn send_cards(&self, runs: &[&HostList], title: (Prelude, Option<ID3D12Resource>), back: Vec<Declared>) -> (Flight, [u128; 2]) {
            let mut submitted = self.submitted.lock().unwrap_or_else(|e| e.into_inner());
            let started = std::time::Instant::now();
            let card = self.card_before(runs, true);
            let mut lists: Vec<Option<ID3D12CommandList>> = vec![Some(title.0.1.cast().unwrap())];
            if let Some(check) = card.as_ref().and_then(|c| c.check.clone()) {
                lists.push(Some(check));
            }
            let prepared = std::time::Instant::now();
            *submitted += 1;

            unsafe {
                self.queue.ExecuteCommandLists(&lists);
                self.queue.Signal(&self.fence, *submitted).unwrap();
            }
            self.card_sent();
            let sent = std::time::Instant::now();
            (
                Flight {
                    card,
                    back,
                    fence: *submitted,
                    _title: title,
                },
                [(prepared - started).as_nanos(), (sent - prepared).as_nanos()],
            )
        }

                          pub(crate) fn results_low(&self) -> bool {
            self.card_results_low()
        }

                                                              pub(crate) fn land_cards(&self, flights: Vec<Flight>) -> [u128; 2] {
            let started = std::time::Instant::now();
            if let Some(last) = flights.last() {

                unsafe {
                    if self.fence.GetCompletedValue() < last.fence {
                        self.fence.SetEventOnCompletion(last.fence, self.event).unwrap();
                        WaitForSingleObject(self.event, INFINITE);
                    }
                }
            }
            let ran = std::time::Instant::now();
            let plans: Vec<&CardPlan> = flights.iter().filter_map(|f| f.card.as_ref()).collect();
            {
                let mut fetch = self.relay_fetch.lock().unwrap_or_else(|e| e.into_inner());
                self.relay_card_back(&plans, &mut fetch);
            }
            let mut back: Vec<Declared> = Vec::new();
            let mut back_index: std::collections::HashMap<usize, (usize, std::collections::HashSet<(usize, usize)>)> =
                std::collections::HashMap::new();
            for flight in &flights {
                for (shadow, ranges) in &flight.back {
                    let key = std::sync::Arc::as_ptr(shadow) as usize;
                    let (idx, seen) = back_index.entry(key).or_insert_with(|| {
                        let idx = back.len();
                        back.push((shadow.clone(), Vec::with_capacity(ranges.len())));
                        (idx, std::collections::HashSet::with_capacity(ranges.len()))
                    });
                    let all = &mut back[*idx].1;
                    for &range in ranges {
                        if seen.insert(range) {
                            all.push(range);
                        }
                    }
                }
            }
            relay_hand_back(&back);
            for flight in &flights {
                for (shadow, ranges) in &flight.back {
                    shadow.copy_back_done(ranges);
                }
            }
            for flight in flights {
                if let Some(card) = flight.card {
                    self.card_done(card);
                }
            }
            let done = std::time::Instant::now();
            [(ran - started).as_nanos(), (done - ran).as_nanos()]
        }

                                   pub(crate) fn title_list(&self, writes: &[(ID3D12Resource, u64, Vec<u8>)]) -> (Prelude, Option<ID3D12Resource>) {
            let prelude = self.make_prelude().expect("a list");
            let total: usize = writes.iter().map(|w| w.2.len()).sum();
            let source = (total > 0).then(|| self.shared_buffer(total as u64).expect("upload memory"));

            unsafe {
                prelude.0.Reset().unwrap();
                prelude.1.Reset(&prelude.0, None).unwrap();
                let mut at = 0usize;
                for (dst, off, bytes) in writes {
                    let (src, data) = source.as_ref().unwrap();
                    std::ptr::copy_nonoverlapping(bytes.as_ptr(), data.add(at), bytes.len());
                    prelude.1.CopyBufferRegion(dst, *off, src, at as u64, bytes.len() as u64);
                    at += bytes.len();
                }
                prelude.1.Close().unwrap();
            }
            (prelude, source.map(|s| s.0))
        }

                          pub(crate) fn read_back(&self, buffer: &ID3D12Resource, size: u64) -> Vec<u8> {
            let (shared, data) = self.shared_buffer(size).expect("read-back memory");
            let prelude = self.make_prelude().expect("a list");
            let mut submitted = self.submitted.lock().unwrap_or_else(|e| e.into_inner());

            unsafe {
                prelude.0.Reset().unwrap();
                prelude.1.Reset(&prelude.0, None).unwrap();
                prelude.1.CopyBufferRegion(&shared, 0, buffer, 0, size);
                prelude.1.Close().unwrap();
                self.queue.ExecuteCommandLists(&[Some(prelude.1.cast().unwrap())]);
                *submitted += 1;
                self.queue.Signal(&self.fence, *submitted).unwrap();
                self.fence.SetEventOnCompletion(*submitted, self.event).unwrap();
                WaitForSingleObject(self.event, INFINITE);
                std::slice::from_raw_parts(data, size as usize).to_vec()
            }
        }
    }
}
}

pub mod shadow {
    use std::sync::atomic::{AtomicU8, AtomicU64, Ordering};
    use std::sync::{Arc, Mutex};

    use std::collections::BTreeMap;

    use windows::Win32::Graphics::Direct3D12::ID3D12Resource;
    use windows::core::Interface;

    use crate::host::{HostList, host};
    use crate::watch::PAGE;

                        pub(crate) struct Shadow {
        pub(crate) buffer: ID3D12Resource,
        data: *mut u8,
        gpu: u64,
        used: AtomicU64,
        asked: [AtomicU64; 2],
        unsized_asks: AtomicU64,
        id: u64,
        rebar: u64,
        start: u64,
        len: u64,
        copied: Mutex<CopiedPages>,
        watched: AtomicU8,
        pending: Mutex<Vec<(usize, usize)>>,
        flights: Mutex<Flights>,
        before: Mutex<Option<Vec<u8>>>,
        kept: Mutex<(Vec<bool>, bool)>,
        card: Option<Card>,
        window: bool,
        place: Option<(windows::Win32::Graphics::Direct3D12::ID3D12Heap, u64)>,
    }

    include!(concat!(env!("OUT_DIR"), "/shadow_reference.rs"));


    unsafe impl Send for Shadow {}
    unsafe impl Sync for Shadow {}

              impl Drop for Shadow {
        fn drop(&mut self) {
            if self.place.is_some() {
                crate::inplace::gone(self.window, self.len);
                return;
            }
            if self.rebar != 0 {
                crate::rebar::released(self.rebar);
            }
            if self.kept.get_mut().unwrap_or_else(|e| e.into_inner()).1 {
                KEPT_BYTES.fetch_sub(self.len, Ordering::Relaxed);
            }
            let window = self.window;
            if let Some(card) = &mut self.card {
                if window {
                    WINDOW_CARD_BYTES.fetch_sub(card.size, Ordering::Relaxed);
                } else {
                    CARD_BYTES[0].fetch_sub(card.size, Ordering::Relaxed);
                }
                if card.before.get_mut().unwrap_or_else(|e| e.into_inner()).is_some() {
                    CARD_BYTES[1].fetch_sub(card_hash_bytes(card.size), Ordering::Relaxed);
                }
            }
        }
    }

                        fn no_buffer() -> ID3D12Resource {
        use std::ffi::c_void;
        use windows::core::{GUID, HRESULT};
        #[repr(C)]
        struct Vtbl {
            query: unsafe extern "system" fn(*mut c_void, *const GUID, *mut *mut c_void) -> HRESULT,
            add_ref: unsafe extern "system" fn(*mut c_void) -> u32,
            release: unsafe extern "system" fn(*mut c_void) -> u32,
        }
        unsafe extern "system" fn query(_: *mut c_void, _: *const GUID, out: *mut *mut c_void) -> HRESULT {

            unsafe { *out = std::ptr::null_mut() };
            HRESULT(0x8000_4002u32 as i32)
        }
        unsafe extern "system" fn one(_: *mut c_void) -> u32 {
            1
        }
        #[repr(C)]
        struct Object {
            vtbl: &'static Vtbl,
        }
        static VTBL: Vtbl = Vtbl { query, add_ref: one, release: one };
        static OBJECT: Object = Object { vtbl: &VTBL };

        unsafe { ID3D12Resource::from_raw(&OBJECT as *const Object as *mut c_void) }
    }

    impl Shadow {
                 pub(crate) fn for_test(data: *mut u8, start: u64, len: u64) -> Shadow {
            let pages = (start + len).div_ceil(PAGE as u64) - start / PAGE as u64;
            Shadow {
                buffer: no_buffer(),
                data,
                gpu: 0,
                used: AtomicU64::new(0),
                asked: [AtomicU64::new(u64::MAX), AtomicU64::new(0)],
                unsized_asks: AtomicU64::new(0),
                id: 0,
                rebar: 0,
                start,
                len,
                copied: Mutex::new(CopiedPages::new(pages as usize)),
                watched: AtomicU8::new(0),
                pending: Mutex::default(),
                flights: Mutex::default(),
                before: Mutex::default(),
                kept: Mutex::default(),
                card: None,
                window: false,
                place: None,
            }
        }

                                                     fn new(buffer: ID3D12Resource, data: *mut u8, gpu: u64, start: u64, len: u64) -> Shadow {
            static NEXT: AtomicU64 = AtomicU64::new(1);
            let mut s = Shadow::for_test(data, start, len);
            s.buffer = buffer;
            s.gpu = gpu;
            s.used = AtomicU64::new(crate::host::moment());
            s.id = NEXT.fetch_add(1, Ordering::Relaxed);
            s
        }

                          fn placed(placed: crate::inplace::Placed, start: u64, len: u64, window: bool) -> Shadow {
            let mut shadow = Shadow::new(placed.buffer, start as *mut u8, placed.gpu, start, len);
            *shadow.watched.get_mut() = NOT_WATCHED;
            shadow.window = window;
            shadow.place = Some((placed.heap, placed.bias));
            shadow
        }

        pub(crate) fn is_placed(&self) -> bool {
            self.place.is_some()
        }

        pub(crate) fn is_window(&self) -> bool {
            self.window
        }

        pub(crate) fn rebar_bytes(&self) -> u64 {
            self.rebar
        }

                 pub(crate) fn card_parts(&self) -> Option<(ID3D12Resource, Option<ID3D12Resource>, u64)> {
            let (_, before, size) = self.card_sides()?;
            Some((self.buffer.clone(), before, size))
        }

                 pub(crate) fn walls_merged(&self, _fetched: usize, _written: usize) {}

        fn refresh_check(&self, _from: usize, _to: usize) {}

        pub(crate) fn start(&self) -> u64 {
            self.start
        }

        pub(crate) fn len(&self) -> u64 {
            self.len
        }

        pub(crate) fn data(&self) -> *mut u8 {
            self.data
        }

                          pub(crate) fn snapshot_hash(&self, ranges: &[(usize, usize)]) -> Option<u64> {
            let before = self.before.lock().unwrap();
            let snap = before.as_ref()?;
            let mut h: u64 = 0xcbf2_9ce4_8422_2325;
            for &(a, b) in ranges {
                for k in a..b {

                    h ^= u64::from(unsafe { *snap.as_ptr().add(k) });
                    h = h.wrapping_mul(0x0000_0100_0000_01b3);
                }
            }
            Some(h)
        }

                          pub(crate) fn state(&self) -> (u8, Vec<u64>, usize, Vec<(usize, usize)>) {
            let copied = self.copied.lock().unwrap();
            (
                self.watched.load(Ordering::Relaxed),
                copied.at.clone(),
                copied.uncopied,
                self.pending.lock().unwrap().clone(),
            )
        }
    }
}

 pub mod rebar {
    include!(concat!(env!("OUT_DIR"), "/rebar.rs"));
}

  pub mod readback {
    use std::sync::Mutex;
    include!(concat!(env!("OUT_DIR"), "/readback_reference.rs"));
}

 pub mod memcopy {
    include!(concat!(env!("OUT_DIR"), "/memcopy_reference.rs"));

         pub(crate) fn hash_every_for_test(value: Option<&str>) -> u64 {
        hash_every_of(value)
    }
}

mod runner;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("--manifest") {
        print!("{}", include_str!(concat!(env!("OUT_DIR"), "/manifest.txt")));
        return;
    }
    if args.len() != 3 {
        eprintln!("usage: oracle-dirty <scenario.txt> <out-dir>\n       oracle-dirty --manifest");
        std::process::exit(2);
    }
    if let Err(e) = runner::run(std::path::Path::new(&args[1]), std::path::Path::new(&args[2])) {
        eprintln!("oracle-dirty: {e}");
        std::process::exit(1);
    }
}
