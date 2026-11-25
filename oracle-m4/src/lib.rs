
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
