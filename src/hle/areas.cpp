// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime API areas (`xgameruntime.rs` `AREAS`; DanziLoader /
// victory-perf, see NOTICE.md), in the reference's order.
#include "areas.hpp"

#include "modules.hpp"
#include "xasync.hpp"
#include "xsystem.hpp"

namespace danzi::hle {

std::vector<Interface> all_area_interfaces() {
    std::vector<Interface> all;
    for (auto list : {xasync::interfaces(), xuser::interfaces(), xsystem::interfaces(), xgame::interfaces(), storage::interfaces(),
                      xnetworking::interfaces(), xpackage::interfaces(), display::interfaces(), xgamesave::interfaces(),
                      xstore::interfaces(), xgameui::interfaces(), speech::interfaces()}) {
        all.insert(all.end(), list.begin(), list.end());
    }
    return all;
}

}   
