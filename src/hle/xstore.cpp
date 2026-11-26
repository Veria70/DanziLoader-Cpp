// SPDX-License-Identifier: GPL-3.0-or-later
// XStore as an offline console answers it (`victory-hle/src/modules/
// xstore.rs`; DanziLoader / victory-perf, see NOTICE.md): the game's own
// licence is local and active (a full, owned game); the user owns no
// add-ons; what needs the store service (catalog, consumables, purchase and
// update pages, collection ids, update checks) fails with the documented
// store network error. Log lines: milestone 7.
#include <cstring>

#include "core/log.hpp"
#include "modules.hpp"
#include "runtime.hpp"
#include "xasync.hpp"

namespace danzi::hle::xstore {

namespace {

using xasync::AsyncBlock;
using This = const Object*;
using Handle = const void*;

const Guid PROVIDER{0x0dd112ac, 0x7c24, 0x448c, {0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3, 0x0c}};


const Guid IID_2026{0xb09d803c, 0x2414, 0x4a05, {0x82, 0xc6, 0x66, 0xdf, 0xdc, 0x9e, 0x9a, 0x44}};

const Guid DURABLES_IID{0x2d42fea5, 0xe71d, 0x4b76, {0x97, 0xcd, 0xc5, 0x0a, 0xfb, 0xb3, 0xae, 0x5d}};


constexpr HRESULT_ NETWORK_ERROR = static_cast<HRESULT_>(0x89245301u);


std::uint8_t CONTEXT = 0, QUERY = 0, VALID = 0, INVALID = 0;

std::uint8_t FAILED = 0, QUERY_CALL = 0, LICENSE_CALL = 0, GAME_LICENSE = 0;

HRESULT_ offline(AsyncBlock* block, const char* what) {
    log::info("victory_hle::modules::xstore", "store call: offline", {{"what", log::debug(what)}});
    return xasync::finished(block, &FAILED, NETWORK_ERROR);
}

HRESULT_ __stdcall status(This, AsyncBlock* block) { return xasync::XAsyncGetStatus(block, false); }

HRESULT_ __stdcall create_context(This, const void*, Handle* out) { return xgr::put(out, static_cast<Handle>(&CONTEXT)); }
void __stdcall close(This, Handle) {}

HRESULT_ __stdcall associated_products_async(This, Handle, std::uint32_t, std::uint32_t, AsyncBlock* block) { return offline(block, "associated products"); }

HRESULT_ __stdcall entitled_products_async(This, Handle, std::uint32_t, std::uint32_t, AsyncBlock* block) {
    return xasync::finished_with(block, &QUERY_CALL, xasync::bytes(static_cast<Handle>(&QUERY)));
}
HRESULT_ __stdcall query_result(This, AsyncBlock* block, Handle* out) { return xasync::result(block, &QUERY_CALL, out); }

HRESULT_ __stdcall enumerate_products(This, Handle, void*, const void*) { return S_OK_; }
bool __stdcall has_more_pages(This, Handle) { return false; }

HRESULT_ __stdcall next_page_async(This, Handle, AsyncBlock* block) { return xasync::finished(block, &QUERY_CALL, E_ILLEGAL_METHOD_CALL_); }

HRESULT_ license_for(bool owned, AsyncBlock* block) {
    Handle license = owned ? &VALID : &INVALID;
    return xasync::finished_with(block, &LICENSE_CALL, xasync::bytes(license));
}

HRESULT_ __stdcall license_for_package_async(This, Handle, const char* package, AsyncBlock* block) {
    auto p = xgr::text(package);
    return license_for(p && *p == xpackage::identifier(), block);
}

HRESULT_ __stdcall license_for_durables_async(This, Handle, const char*, AsyncBlock* block) { return license_for(false, block); }
HRESULT_ __stdcall license_result(This, AsyncBlock* block, Handle* out) { return xasync::result(block, &LICENSE_CALL, out); }
bool __stdcall is_license_valid(This, Handle license) { return license == &VALID; }


struct GameLicense {
    char sku_store_id[18];
    bool is_active, is_trial_owned_by_this_user, is_disc_license, is_trial;
    std::uint32_t trial_seconds_remaining;
    char trial_unique_id[64];
    std::int64_t expiration;
};
static_assert(sizeof(GameLicense) == 104);


GameLicense game_license() {
    GameLicense l{};
    std::string id = runtime::title().store_id + "/0010";
    std::memcpy(l.sku_store_id, id.data(), std::min(id.size(), sizeof l.sku_store_id - 1));
    l.is_active = true;
    return l;
}
HRESULT_ __stdcall game_license_async(This, Handle, AsyncBlock* block) { return xasync::finished_with(block, &GAME_LICENSE, xasync::bytes(game_license())); }
HRESULT_ __stdcall game_license_result(This, AsyncBlock* block, GameLicense* out) { return xasync::result(block, &GAME_LICENSE, out); }

HRESULT_ __stdcall consumable_balance_async(This, Handle, const char*, AsyncBlock* block) { return offline(block, "consumable balance"); }
HRESULT_ __stdcall report_fulfillment_async(This, Handle, const char*, std::uint32_t, const void*, AsyncBlock* block) { return offline(block, "consumable fulfillment"); }

HRESULT_ __stdcall user_id_async(This, Handle, const char*, const char*, AsyncBlock* block) { return offline(block, "user store id"); }
HRESULT_ __stdcall size_status(This, AsyncBlock* block, std::size_t*) { return xasync::XAsyncGetStatus(block, false); }
HRESULT_ __stdcall string_status(This, AsyncBlock* block, std::size_t, char*) { return xasync::XAsyncGetStatus(block, false); }
HRESULT_ __stdcall count_status(This, AsyncBlock* block, std::uint32_t*) { return xasync::XAsyncGetStatus(block, false); }
HRESULT_ __stdcall array_status(This, AsyncBlock* block, std::uint32_t, void*) { return xasync::XAsyncGetStatus(block, false); }

HRESULT_ __stdcall purchase_ui_async(This, Handle, const char*, const char*, const char*, AsyncBlock* block) { return offline(block, "purchase page"); }
HRESULT_ __stdcall product_page_ui_async(This, Handle, const char*, AsyncBlock* block) { return offline(block, "product page"); }
HRESULT_ __stdcall redeem_ui_async(This, Handle, const char*, const char* const*, std::size_t, bool, AsyncBlock* block) { return offline(block, "redeem page"); }
HRESULT_ __stdcall updates_async(This, Handle, AsyncBlock* block) { return offline(block, "update check"); }

HRESULT_ __stdcall install_async(This, Handle, const char* const*, std::size_t, AsyncBlock* block) { return offline(block, "package download"); }


HRESULT_ __stdcall package_identifier(This, const char* store_id, std::size_t size, char* buffer) {
    auto title = runtime::title();
    auto id = xgr::text(store_id);
    if (id && !title.store_id.empty() && *id == title.store_id) {
        return xgr::put_string(xpackage::identifier(), size, buffer, nullptr);
    }
    return xpackage::NO_PACKAGE;
}


HRESULT_ __stdcall reg(This, Handle, const void*, void*, const void*, std::uint64_t* out) { return xgr::put(out, xgr::token()); }
bool __stdcall unreg(This, Handle, std::uint64_t, bool) { return true; }

#define F(x) reinterpret_cast<const void*>(&x)
using xgr::add_ref;
using xgr::missing;
const void* const VTABLE[76] = {
    F(missing<0>),
    F(add_ref),
    F(add_ref),
    F(create_context),              
    F(close),                       
    F(associated_products_async),   
    F(query_result),                
    F(missing<7>),                  
    F(missing<8>),                  
    F(entitled_products_async),     
    F(query_result),                
    F(missing<11>),                 
    F(missing<12>),                 
    F(missing<13>),                 
    F(missing<14>),                 
    F(enumerate_products),          
    F(has_more_pages),              
    F(next_page_async),             
    F(query_result),                
    F(close),                       
    F(license_for_package_async),   
    F(license_result),              
    F(is_license_valid),            
    F(close),                       
    F(missing<24>),                 
    F(missing<25>),                 
    F(missing<26>),                 
    F(missing<27>),                 
    F(game_license_async),          
    F(game_license_result),         
    F(missing<30>),                 
    F(missing<31>),                 
    F(missing<32>),                 
    F(consumable_balance_async),    
    F(status),                      
    F(report_fulfillment_async),    
    F(missing<36>),                 
    F(user_id_async),               
    F(size_status),                 
    F(string_status),               
    F(user_id_async),               
    F(size_status),                 
    F(string_status),               
    F(missing<43>),                 
    F(missing<44>),                 
    F(missing<45>),                 
    F(missing<46>),                 
    F(missing<47>),                 
    F(missing<48>),                 
    F(purchase_ui_async),           
    F(missing<50>),                 
    F(missing<51>),                 
    F(missing<52>),                 
    F(redeem_ui_async),             
    F(missing<54>),                 
    F(updates_async),               
    F(count_status),                
    F(array_status),                
    F(missing<58>),                 
    F(missing<59>),                 
    F(install_async),               
    F(status),                      
    F(install_async),               
    F(count_status),                
    F(array_status),                
    F(package_identifier),          
    F(reg),                         
    F(unreg),                       
    F(reg),                         
    F(unreg),                       
    F(missing<70>),                 
    F(license_for_durables_async),  
    F(license_result),              
    F(missing<73>),                 
    F(missing<74>),                 
    F(product_page_ui_async),       
};
#undef F
const Object OBJECT{VTABLE, "store"};

}   

std::vector<Interface> interfaces() { return {{PROVIDER, IID_2026, &OBJECT}, {PROVIDER, DURABLES_IID, &OBJECT}}; }

}   
