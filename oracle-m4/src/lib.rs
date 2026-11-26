
use std::ffi::c_void;
use std::sync::OnceLock;

use victory_gpu::object::{HRESULT, Iid, Object};

type Alloc = extern "system" fn(usize, u64) -> *mut c_void;
type Free = extern "system" fn(*mut c_void, u64);
type Mapper = extern "system" fn(u64, u64, u64) -> i32;

static MAPPER: OnceLock<Mapper> = OnceLock::new();

  fn map_pages(to: u64, from: u64, len: u64) -> bool {
    MAPPER.get().is_some_and(|m| m(to, from, len) != 0)
}

  #[unsafe(no_mangle)]
pub extern "system" fn backend_use_runtime(alloc: Alloc, free: Free, mapper: Mapper) {

    if let Ok(filter) = std::env::var("ORACLE_LOG") {
        let _ = tracing_subscriber::fmt()
            .with_env_filter(tracing_subscriber::EnvFilter::new(filter))
            .with_writer(std::io::stderr)
            .with_ansi(false)
            .try_init();
    }

    victory_gpu::resource::use_allocator(alloc, free);
    let _ = MAPPER.set(mapper);
    victory_gpu::resource::use_page_mapper(map_pages);
}

#[unsafe(no_mangle)]
pub extern "system" fn backend_D3D12ConfigureVirtualMemoryX(config: *mut victory_gpu::device::VirtualMemory) -> HRESULT {
    victory_gpu::device::configure_virtual_memory(config)
}

#[unsafe(no_mangle)]
pub extern "system" fn backend_D3D12XboxCreateDevice(adapter: *const c_void, parameters: *const u32, iid: *const Iid, out: *mut *const Object) -> HRESULT {
    victory_gpu::device::create(adapter, parameters, iid, out)
}

#[unsafe(no_mangle)]
pub extern "system" fn backend_D3D12SerializeRootSignature(
    desc: *const victory_gpu::root::Desc,
    version: u32,
    blob: *mut *mut c_void,
    error: *mut *mut c_void,
) -> HRESULT {
    victory_gpu::root::serialize(desc, version, blob, error)
}

#[unsafe(no_mangle)]
pub extern "system" fn backend_D3D12SerializeVersionedRootSignature(
    desc: *const victory_gpu::root::VersionedDesc,
    blob: *mut *mut c_void,
    error: *mut *mut c_void,
) -> HRESULT {
    victory_gpu::root::serialize_versioned(desc, blob, error)
}

#[unsafe(no_mangle)]
pub extern "system" fn backend_D3D12CreateRootSignatureDeserializer(blob: *const u8, size: usize, iid: *const Iid, out: *mut *mut c_void) -> HRESULT {
    victory_gpu::root::create_deserializer(blob, size, iid, out)
}

 #[unsafe(no_mangle)]
pub extern "system" fn backend_device_iid() -> *const Iid {
    &raw const victory_gpu::device::IID
}

  #[unsafe(no_mangle)]
pub extern "system" fn backend_set_console(series_s: u32) {
    victory_gpu::console::set(if series_s != 0 { victory_gpu::console::Console::SeriesS } else { victory_gpu::console::Console::SeriesX });
}

 #[unsafe(no_mangle)]
pub extern "system" fn backend_title_memory_remapped(at: usize, len: usize) {
    victory_gpu::title_memory_remapped(at, len);
}


   #[repr(C)]
pub struct LayoutQuery {
    dimension: u32,
    width: u32,
    height: u32,
    depth_or_array: u32,
    mips: u32,
    format: u32,
    samples: u32,
    swizzle: u32,
}

fn texture_desc(q: &LayoutQuery) -> Option<victory_formats::texture::TextureDesc> {
    use victory_formats::texture::{Dimension, Swizzle, TextureDesc};
    Some(TextureDesc {
        dimension: match q.dimension {
            0 => Dimension::Tex1d,
            1 => Dimension::Tex2d,
            2 => Dimension::Tex3d,
            _ => return None,
        },
        width: q.width,
        height: q.height,
        depth_or_array: q.depth_or_array,
        mips: q.mips,
        format: q.format,
        samples: q.samples,
        swizzle: Swizzle(q.swizzle),
    })
}

   #[unsafe(no_mangle)]
pub extern "system" fn oracle_texture_layout(query: *const LayoutQuery, out: *mut u8, room: usize) -> usize {
    use std::fmt::Write;

    let q = unsafe { &*query };
    let mut s = String::new();
    match texture_desc(q).map(|d| victory_formats::texture::Texture::new(&d)) {
        None => s.push_str("no dimension\n"),
        Some(Err(e)) => {
            let _ = writeln!(s, "error {e:?}");
        }
        Some(Ok(t)) => {
            let _ = writeln!(s, "size {} alignment {} planes {}", t.size, t.alignment, t.planes.len());
            for p in &t.planes {
                let l = &p.layout;
                let _ = writeln!(
                    s,
                    "plane offset {} stencil {} block {:?} thick {} slices {} slice_size {} size {} alignment {} first_tail_mip {} addressable {}",
                    p.offset,
                    p.stencil,
                    l.block,
                    l.thick,
                    l.slices,
                    l.slice_size,
                    l.size,
                    l.alignment,
                    l.first_tail_mip,
                    l.addressable()
                );
                for (i, m) in l.mips.iter().enumerate() {
                    let tail = m.tail.map_or("-".to_string(), |t| format!("{},{},{}", t.offset, t.x, t.y));
                    let _ = writeln!(
                        s,
                        " mip {i} {}x{}x{} pitch {} rows {} depth_pitch {} offset {} tail {tail} bytes_from {}",
                        m.width,
                        m.height,
                        m.depth,
                        m.pitch,
                        m.rows,
                        m.depth_pitch,
                        m.offset,
                        l.bytes_from(i as u32)
                    );

                    let (w, h) = (m.width, m.height);
                    for (x, y) in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1), (w / 2, h / 2)] {
                        match l.offset(i as u32, x, y, 0) {
                            Ok(o) => {
                                let _ = write!(s, " {o}");
                            }
                            Err(e) => {
                                let _ = write!(s, " {e:?}");
                            }
                        }
                    }
                    let last = if q.dimension == 2 { m.depth } else { l.slices }.max(1) - 1;
                    match l.offset(i as u32, 0, 0, last) {
                        Ok(o) => {
                            let _ = writeln!(s, " z{last} {o}");
                        }
                        Err(e) => {
                            let _ = writeln!(s, " z{last} {e:?}");
                        }
                    }
                }
            }
        }
    }
    let n = s.len().min(room);

    unsafe { std::ptr::copy_nonoverlapping(s.as_ptr(), out, n) };
    s.len()
}

   #[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub extern "system" fn oracle_texture_copy(
    query: *const LayoutQuery,
    plane: u32,
    tile: u32,
    tiled: *mut u8,
    tiled_len: usize,
    mip: u32,
    slice: u32,
    linear: *mut u8,
    linear_len: usize,
    row_pitch: usize,
    depth_pitch: usize,
) -> u32 {
    use victory_formats::texture::Error;

    let q = unsafe { &*query };
    let Some(Ok(t)) = texture_desc(q).map(|d| victory_formats::texture::Texture::new(&d)) else {
        return 4;
    };
    let Some(p) = t.planes.get(plane as usize) else {
        return 4;
    };

    let (tiled, linear) = unsafe {
        (
            std::slice::from_raw_parts_mut(tiled, tiled_len),
            std::slice::from_raw_parts_mut(linear, linear_len),
        )
    };
    let r = if tile != 0 {
        p.layout.tile(tiled, mip, slice, linear, row_pitch, depth_pitch)
    } else {
        p.layout.detile(tiled, mip, slice, linear, row_pitch, depth_pitch)
    };
    match r {
        Ok(()) => 0,
        Err(Error::Invalid) => 1,
        Err(Error::Unsupported) => 2,
        Err(Error::TooSmall) => 3,
    }
}

   #[unsafe(no_mangle)]
pub extern "system" fn oracle_texture_size(query: *const LayoutQuery) -> u64 {

    let q = unsafe { &*query };
    match texture_desc(q).map(|d| victory_formats::texture::Texture::new(&d)) {
        Some(Ok(t)) => t.size,
        _ => u64::MAX,
    }
}


#[path = "../../crates/victory-gpu/src/sampler.rs"]
#[allow(dead_code)]
mod sampler;

  #[unsafe(no_mangle)]
pub extern "system" fn oracle_console_sampler(words: *const [u32; 27], out: *mut [u32; 13]) {

    let d = sampler::console(unsafe { &*words });

    unsafe { out.write(std::mem::transmute::<windows::Win32::Graphics::Direct3D12::D3D12_SAMPLER_DESC, [u32; 13]>(d)) };
}

  #[unsafe(no_mangle)]
pub extern "system" fn backend_use_shader_cache(dir: *const u16) {
    if dir.is_null() {
        return;
    }

    let len = (0..).take_while(|&i| unsafe { *dir.add(i) } != 0).count();
    let wide = unsafe { std::slice::from_raw_parts(dir, len) };
    let path = std::path::PathBuf::from(String::from_utf16_lossy(wide));
    victory_gpu::shader_cache::use_shader_cache(path);
}

     #[unsafe(no_mangle)]
pub extern "system" fn oracle_shader_build(out: *mut [u8; 32]) {
    let limit = victory_shader::emit::loop_limit();
    let mut hash = blake3::Hasher::new();
    for source in victory_shader::sources()
        .iter()
        .copied()
        .chain([include_str!("../../crates/victory-gpu/src/shader_cache.rs")])
    {
        hash.update(&(source.len() as u64).to_le_bytes());
        hash.update(source.as_bytes());
    }
    hash.update(env!("CARGO_PKG_VERSION").as_bytes());
    hash.update(&[0u8]);
    for n in [
        limit,
        victory_shader::emit::view_heap_limit(),
        victory_shader::emit::host_max_wave(),
        u32::from(victory_shader::emit::group_sync()),
        victory_shader::emit::shader_opt(),
    ] {
        hash.update(&n.to_le_bytes());
    }
    for name in ["VICTORY_WAVE_WIDE", "VICTORY_WAVE_DEEP_SKIPS", "VICTORY_WAVE_OWN_BITS", "VICTORY_F16_NEAREST"] {
        hash.update(name.as_bytes());
        hash.update(b"=");
        if let Some(value) = std::env::var_os(name) {
            hash.update(value.as_encoded_bytes());
        }
        hash.update(&[0]);
    }

    unsafe { *out = *hash.finalize().as_bytes() };
}

 #[unsafe(no_mangle)]
pub extern "system" fn backend_use_pipeline_cache(dir: *const u16) {
    if dir.is_null() {
        return;
    }

    let len = (0..).take_while(|&i| unsafe { *dir.add(i) } != 0).count();
    let wide = unsafe { std::slice::from_raw_parts(dir, len) };
    victory_gpu::pipeline::use_pipeline_cache(std::path::PathBuf::from(String::from_utf16_lossy(wide)));
}

#[unsafe(no_mangle)]
pub extern "system" fn backend_save_pipeline_cache() {
    victory_gpu::pipeline::save_pipeline_cache();
}

   #[unsafe(no_mangle)]
pub extern "system" fn backend_precompile_init(out: *mut u32) -> i32 {
    match victory_gpu::precompile::init_host() {
        Ok(info) => {

            unsafe {
                *out = info.view_heap_limit;
                *out.add(1) = u32::from(info.mesh_shaders);
            }
            0
        }
        Err(_) => -1,
    }
}

static PREPARED: std::sync::Mutex<Vec<victory_gpu::precompile::PreparedShader>> = std::sync::Mutex::new(Vec::new());

    #[unsafe(no_mangle)]
pub extern "system" fn backend_precompile_shader(container: *const u8, len: usize, out: *mut u32) -> i32 {

    let bytes = unsafe { std::slice::from_raw_parts(container, len) };
    match victory_gpu::precompile::shader(bytes) {
        Ok(prepared) => {
            let mut name = [0u8; 16];
            let n = prepared.stage.len().min(16);
            name[..n].copy_from_slice(&prepared.stage.as_bytes()[..n]);
            let mut list = PREPARED.lock().unwrap_or_else(|e| e.into_inner());

            unsafe {
                std::ptr::copy_nonoverlapping(name.as_ptr(), out.cast::<u8>(), 16);
                *out.add(4) = u32::from(prepared.is_compute);
                *out.add(5) = u32::from(prepared.cached);
                *out.add(6) = list.len() as u32;
            }
            list.push(prepared);
            0
        }
        Err(_) => -1,
    }
}

  #[unsafe(no_mangle)]
pub extern "system" fn backend_precompile_compute(id: u32, label: *const u8, label_len: usize) -> i32 {

    let label = String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(label, label_len) }).into_owned();
    let list = PREPARED.lock().unwrap_or_else(|e| e.into_inner());
    let Some(prepared) = list.get(id as usize) else { return -1 };
    match victory_gpu::precompile::compute_pipeline(prepared, &label) {
        Ok(loaded) => i32::from(loaded),
        Err(_) => -1,
    }
}
