// SPDX-License-Identifier: GPL-3.0-or-later
// A console without a network cable: the title reaches loopback only
// (`victory-hle/src/modules/net.rs`; DanziLoader / victory-perf, see
// NOTICE.md).
//
// Every way out of Winsock and WinHTTP a title can import or look up by name
// is vetted here; the rest of both DLLs is the host's own.
// - Connects (also by name, by list, `ConnectEx` from `WSAIoctl`) and
//   datagram sends to anything but 127.0.0.0/8 and ::1 fail with
//   `WSAENETUNREACH`.
// - Forward lookups other than `localhost` and IP literals fail with
//   `WSAHOST_NOT_FOUND`; reverse lookups of other addresses come back
//   numeric, or fail if a name is required, as with no network.
// - `WinHttpConnect` to a host that is not loopback fails with
//   `ERROR_WINHTTP_CANNOT_CONNECT`, so no request (or WebSocket) handle can
//   exist for one. Proxies are off and proxy auto-detection (WPAD, PAC
//   downloads) fails with `ERROR_WINHTTP_AUTODETECTION_FAILED`.
// - A bind to all interfaces is narrowed to loopback, so nothing on the
//   network can connect in.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "modules.hpp"
#include "registry.hpp"

namespace danzi::hle::net {

namespace {

using Socket = std::uintptr_t;
using Handle = void*;

constexpr int SOCKET_ERROR_ = -1;
constexpr Socket INVALID_SOCKET_ = ~Socket{0};
constexpr int WSAEINVAL_ = 10022;
constexpr int WSAEADDRNOTAVAIL_ = 10049;
constexpr int WSAENETUNREACH_ = 10051;
constexpr int WSASERVICE_NOT_FOUND_ = 10108;
constexpr int WSAHOST_NOT_FOUND_ = 11001;
constexpr DWORD ERROR_WINHTTP_CANNOT_CONNECT_ = 12029;
constexpr DWORD ERROR_WINHTTP_AUTODETECTION_FAILED_ = 12180;
constexpr std::uint16_t AF_INET_ = 2;
constexpr std::uint16_t AF_INET6_ = 23;
constexpr int NI_NUMERICHOST_ = 0x02;
constexpr int NI_NAMEREQD_ = 0x04;
constexpr std::uint32_t SIO_GET_EXTENSION_FUNCTION_POINTER_ = 0xC8000006u;
constexpr std::uint32_t SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER_ = 0xC8000024u;
constexpr std::uint32_t WINHTTP_ACCESS_TYPE_NO_PROXY_ = 1;
constexpr std::uint32_t WINHTTP_OPTION_PROXY_ = 38;

const Guid WSAID_CONNECTEX_{0x25a207b9, 0xddf3, 0x4660, {0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}};
const Guid WSAID_WSASENDMSG_{0xa441e712, 0x754f, 0x43ca, {0x84, 0xa7, 0x0d, 0xee, 0x44, 0xcf, 0x60, 0x6d}};

using Ioctl = int(WINAPI*)(Socket, std::uint32_t, const void*, std::uint32_t, void*, std::uint32_t, std::uint32_t*, void*, const void*);

void wsa_error(int code) { HOST("ws2_32.dll", "WSASetLastError", void(WINAPI*)(int))(code); }

std::optional<std::string> narrow(const char* text) { return xgr::text(text); }

std::optional<std::string> wide(const wchar_t* text) {
    if (!text) {
        return std::nullopt;
    }
    return xgr::utf16_lossy(text);
}


struct Ip {
    bool v6 = false;
    std::uint8_t b[16] = {};
};


class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::optional<Ip> ip() {
        std::optional<Ip> r = v4();
        if (!r) {
            r = v6();
        }
        if (!r || pos_ != s_.size()) {
            return std::nullopt;
        }
        return r;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;

    bool given(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    static int digit(char c, std::uint32_t radix) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (radix == 16 && c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (radix == 16 && c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }


    std::optional<std::uint32_t> number(std::uint32_t radix, std::size_t max_digits, bool allow_zero_prefix, std::uint32_t max) {
        std::size_t save = pos_;
        bool leading_zero = pos_ < s_.size() && s_[pos_] == '0';
        std::uint32_t result = 0;
        std::size_t count = 0;
        while (pos_ < s_.size()) {
            int d = digit(s_[pos_], radix);
            if (d < 0) {
                break;
            }
            ++pos_;
            result = result * radix + static_cast<std::uint32_t>(d);
            ++count;
            if (count > max_digits) {
                pos_ = save;
                return std::nullopt;
            }
        }
        if (count == 0 || result > max || (!allow_zero_prefix && leading_zero && count > 1)) {
            pos_ = save;
            return std::nullopt;
        }
        return result;
    }

    std::optional<Ip> v4() {
        std::size_t save = pos_;
        Ip ip;
        for (std::size_t i = 0; i < 4; ++i) {
            std::optional<std::uint32_t> part;
            if (i == 0 || given('.')) {
                part = number(10, 3, false, 0xFF);
            }
            if (!part) {
                pos_ = save;
                return std::nullopt;
            }
            ip.b[i] = static_cast<std::uint8_t>(*part);
        }
        return ip;
    }


    std::pair<std::size_t, bool> groups(std::uint16_t* out, std::size_t limit) {
        for (std::size_t i = 0; i < limit; ++i) {

            if (i < limit - 1) {
                std::size_t save = pos_;
                std::optional<Ip> four;
                if (i == 0 || given(':')) {
                    four = v4();
                }
                if (four) {
                    out[i] = static_cast<std::uint16_t>(four->b[0] << 8 | four->b[1]);
                    out[i + 1] = static_cast<std::uint16_t>(four->b[2] << 8 | four->b[3]);
                    return {i + 2, true};
                }
                pos_ = save;
            }
            std::size_t save = pos_;
            std::optional<std::uint32_t> group;
            if (i == 0 || given(':')) {
                group = number(16, 4, true, 0xFFFF);
            }
            if (!group) {
                pos_ = save;
                return {i, false};
            }
            out[i] = static_cast<std::uint16_t>(*group);
        }
        return {limit, false};
    }

    std::optional<Ip> v6() {
        std::size_t save = pos_;
        std::uint16_t head[8] = {};
        auto finish = [](const std::uint16_t* g) {
            Ip ip;
            ip.v6 = true;
            for (int i = 0; i < 8; ++i) {
                ip.b[2 * i] = static_cast<std::uint8_t>(g[i] >> 8);
                ip.b[2 * i + 1] = static_cast<std::uint8_t>(g[i]);
            }
            return ip;
        };
        auto [head_size, head_v4] = groups(head, 8);
        if (head_size == 8) {
            return finish(head);
        }

        if (head_v4 || !given(':') || !given(':')) {
            pos_ = save;
            return std::nullopt;
        }

        std::uint16_t tail[7] = {};
        auto [tail_size, tail_v4] = groups(tail, 8 - (head_size + 1));
        (void)tail_v4;
        for (std::size_t i = 0; i < tail_size; ++i) {
            head[8 - tail_size + i] = tail[i];
        }
        return finish(head);
    }
};

std::optional<Ip> parse_ip(std::string_view s) { return Parser(s).ip(); }

bool loopback(const Ip& ip) {
    if (!ip.v6) {
        return ip.b[0] == 127;
    }

    static const std::uint8_t MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(ip.b, MAPPED, 12) == 0) {
        return ip.b[12] == 127;
    }
    static const std::uint8_t ONE[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return std::memcmp(ip.b, ONE, 16) == 0;
}

bool unspecified(const Ip& ip) {
    for (int i = 0; i < (ip.v6 ? 16 : 4); ++i) {
        if (ip.b[i] != 0) {
            return false;
        }
    }
    return true;
}

bool localhost(std::string_view s) {
    constexpr std::string_view L = "localhost";
    if (s.size() != L.size()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        if (c != L[i]) {
            return false;
        }
    }
    return true;
}


std::string show_ip(const Ip& ip) {
    char part[16];
    auto v4 = [&](const std::uint8_t* b) {
        std::snprintf(part, sizeof part, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return std::string(part);
    };
    if (!ip.v6) {
        return v4(ip.b);
    }
    static const std::uint8_t MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(ip.b, MAPPED, 12) == 0) {
        return "::ffff:" + v4(ip.b + 12);
    }
    std::uint16_t g[8];
    for (int i = 0; i < 8; ++i) {
        g[i] = static_cast<std::uint16_t>(ip.b[2 * i] << 8 | ip.b[2 * i + 1]);
    }
    int best_start = 0, best_len = 0, start = 0, len = 0;
    for (int i = 0; i < 8; ++i) {
        if (g[i] == 0) {
            if (len == 0) {
                start = i;
            }
            ++len;
            if (len > best_len) {
                best_start = start;
                best_len = len;
            }
        } else {
            len = 0;
        }
    }
    auto groups = [&](int from, int to) {
        std::string s;
        for (int i = from; i < to; ++i) {
            std::snprintf(part, sizeof part, i == from ? "%x" : ":%x", g[i]);
            s += part;
        }
        return s;
    };
    if (best_len > 1) {
        return groups(0, best_start) + "::" + groups(best_start + best_len, 8);
    }
    return groups(0, 8);
}


struct SockAddr {
    Ip ip;
    std::uint16_t port = 0;
};


std::string show_addr(const SockAddr& a) {
    std::string port = std::to_string(a.port);
    return a.ip.v6 ? "[" + show_ip(a.ip) + "]:" + port : show_ip(a.ip) + ":" + port;
}

std::string shown(const std::optional<SockAddr>& a) { return a ? show_addr(*a) : "not an IP address"; }
std::string shown(const std::optional<Ip>& a) { return a ? show_ip(*a) : "not an IP address"; }


void once(const char* what, std::string destination) {
    static std::mutex lock;
    static std::vector<std::pair<std::string, std::string>> seen;
    std::lock_guard l(lock);
    for (const auto& [w, d] : seen) {
        if (w == what && d == destination) {
            return;
        }
    }
    log::info("victory_hle::modules::net", what, {{"destination", destination}});
    seen.emplace_back(what, std::move(destination));
}


std::optional<SockAddr> socket_addr(const std::uint8_t* addr, int len) {
    if (len < 0 || !addr || len < 4) {
        return std::nullopt;
    }
    std::uint16_t family = static_cast<std::uint16_t>(addr[0] | addr[1] << 8);
    SockAddr a;
    a.port = static_cast<std::uint16_t>(addr[2] << 8 | addr[3]);
    if (family == AF_INET_ && len >= 16) {
        std::memcpy(a.ip.b, addr + 4, 4);
        return a;
    }
    if (family == AF_INET6_ && len >= 28) {
        a.ip.v6 = true;
        std::memcpy(a.ip.b, addr + 8, 16);
        return a;
    }
    return std::nullopt;
}


std::optional<Ip> raw_ip(const char* addr, int len, int kind) {
    if (len < 0 || !addr || kind < 0 || kind > 0xFFFF) {
        return std::nullopt;
    }
    Ip ip;
    if (kind == AF_INET_ && len >= 4) {
        std::memcpy(ip.b, addr, 4);
        return ip;
    }
    if (kind == AF_INET6_ && len >= 16) {
        ip.v6 = true;
        std::memcpy(ip.b, addr, 16);
        return ip;
    }
    return std::nullopt;
}


bool loopback_name(std::string_view name) {
    while (!name.empty() && name.front() == '[') {
        name.remove_prefix(1);
    }
    while (!name.empty() && name.back() == ']') {
        name.remove_suffix(1);
    }
    if (localhost(name)) {
        return true;
    }
    auto ip = parse_ip(name);
    return ip && loopback(*ip);
}


bool reachable(const char* what, const std::uint8_t* addr, int len) {
    auto a = socket_addr(addr, len);
    if (a && loopback(a->ip)) {
        return true;
    }
    once(what, shown(a));
    wsa_error(WSAENETUNREACH_);
    return false;
}


bool reachable_name(const std::optional<std::string>& name) {
    if (name && !loopback_name(*name)) {
        bool literal = parse_ip(*name).has_value();
        once("network connect blocked", *name);
        wsa_error(literal ? WSAENETUNREACH_ : WSAHOST_NOT_FOUND_);
        return false;
    }

    return true;
}


bool resolvable(const std::optional<std::string>& name) {
    if (name && !(name->empty() || localhost(*name) || parse_ip(*name))) {
        once("network name lookup blocked", *name);
        wsa_error(WSAHOST_NOT_FOUND_);
        return false;
    }
    return true;
}


bool reverse_resolvable(const std::optional<Ip>& ip) {
    if (ip && loopback(*ip)) {
        return true;
    }
    once("network reverse lookup blocked", shown(ip));
    return false;
}

std::optional<Ip> ip_of(const std::optional<SockAddr>& a) { return a ? std::optional(a->ip) : std::nullopt; }


std::optional<int> name_info_flags(const std::uint8_t* sa, int len, bool host_wanted, int flags) {
    if (!host_wanted || (flags & NI_NUMERICHOST_) != 0 || reverse_resolvable(ip_of(socket_addr(sa, len)))) {
        return flags;
    }
    if ((flags & NI_NAMEREQD_) != 0) {
        wsa_error(WSAHOST_NOT_FOUND_);
        return std::nullopt;
    }
    return flags | NI_NUMERICHOST_;
}


bool stream(Socket s) {
    constexpr int SOL_SOCKET_ = 0xffff;
    constexpr int SO_TYPE_ = 0x1008;
    constexpr int SOCK_STREAM_ = 1;
    int kind = 0, len = 4;
    auto getsockopt = HOST("ws2_32.dll", "getsockopt", int(WINAPI*)(Socket, int, int, int*, int*));
    return getsockopt(s, SOL_SOCKET_, SO_TYPE_, &kind, &len) == 0 && kind == SOCK_STREAM_;
}

}   


int WINAPI connect(Socket s, const std::uint8_t* name, int len) {
    if (!reachable("network connect blocked", name, len)) {
        return SOCKET_ERROR_;
    }
    return HOST("ws2_32.dll", "connect", int(WINAPI*)(Socket, const std::uint8_t*, int))(s, name, len);
}

int WINAPI WSAConnect(Socket s, const std::uint8_t* name, int len, const void* caller_data, void* callee_data, const void* sqos, const void* gqos) {
    if (!reachable("network connect blocked", name, len)) {
        return SOCKET_ERROR_;
    }
    return HOST("ws2_32.dll", "WSAConnect", int(WINAPI*)(Socket, const std::uint8_t*, int, const void*, void*, const void*, const void*))(s, name, len, caller_data, callee_data, sqos, gqos);
}

BOOL WINAPI WSAConnectByNameA(Socket s, const char* node, const char* service, std::uint32_t* local_len, void* local, std::uint32_t* remote_len, void* remote, const void* timeout, void* reserved) {
    if (!reachable_name(narrow(node))) {
        return 0;
    }
    return HOST("ws2_32.dll", "WSAConnectByNameA", BOOL(WINAPI*)(Socket, const char*, const char*, std::uint32_t*, void*, std::uint32_t*, void*, const void*, void*))(s, node, service, local_len, local, remote_len, remote, timeout, reserved);
}

BOOL WINAPI WSAConnectByNameW(Socket s, const wchar_t* node, const wchar_t* service, std::uint32_t* local_len, void* local, std::uint32_t* remote_len, void* remote, const void* timeout, void* reserved) {
    if (!reachable_name(wide(node))) {
        return 0;
    }
    return HOST("ws2_32.dll", "WSAConnectByNameW", BOOL(WINAPI*)(Socket, const wchar_t*, const wchar_t*, std::uint32_t*, void*, std::uint32_t*, void*, const void*, void*))(s, node, service, local_len, local, remote_len, remote, timeout, reserved);
}

namespace {


struct SocketAddress {
    const std::uint8_t* addr;
    int len;
};
static_assert(sizeof(SocketAddress) == 16);
}   


BOOL WINAPI WSAConnectByList(Socket s, const std::uint8_t* list, std::uint32_t* local_len, void* local, std::uint32_t* remote_len, void* remote, const void* timeout, void* reserved) {
    if (list) {
        std::int32_t count = 0;
        std::memcpy(&count, list, 4);
        auto entries = reinterpret_cast<const SocketAddress*>(list + 8);
        for (std::int32_t i = 0; i < count; ++i) {
            if (!reachable("network connect blocked", entries[i].addr, entries[i].len)) {
                return 0;
            }
        }
    }
    return HOST("ws2_32.dll", "WSAConnectByList", BOOL(WINAPI*)(Socket, const std::uint8_t*, std::uint32_t*, void*, std::uint32_t*, void*, const void*, void*))(s, list, local_len, local, remote_len, remote, timeout, reserved);
}

Socket WINAPI WSAJoinLeaf(Socket s, const std::uint8_t* name, int len, const void* caller_data, void* callee_data, const void* sqos, const void* gqos, std::uint32_t flags) {
    if (!reachable("network connect blocked", name, len)) {
        return INVALID_SOCKET_;
    }
    return HOST("ws2_32.dll", "WSAJoinLeaf", Socket(WINAPI*)(Socket, const std::uint8_t*, int, const void*, void*, const void*, const void*, std::uint32_t))(s, name, len, caller_data, callee_data, sqos, gqos, flags);
}

namespace {

BOOL WINAPI connect_ex(Socket s, const std::uint8_t* name, int len, const void* send, std::uint32_t send_len, std::uint32_t* sent, void* overlapped) {
    if (!reachable("network connect blocked", name, len)) {
        return 0;
    }
    std::uintptr_t real = 0;
    std::uint32_t size = 0;
    auto ioctl = HOST("ws2_32.dll", "WSAIoctl", Ioctl);
    Guid guid = WSAID_CONNECTEX_;
    int found = ioctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER_, &guid, 16, &real, 8, &size, nullptr, nullptr);
    if (found != 0 || real == 0) {
        return 0;
    }

    auto f = reinterpret_cast<BOOL(WINAPI*)(Socket, const std::uint8_t*, int, const void*, std::uint32_t, std::uint32_t*, void*)>(real);
    return f(s, name, len, send, send_len, sent, overlapped);
}
}   

int WINAPI WSASendMsg(Socket s, const void* message, std::uint32_t flags, std::uint32_t* sent, void* overlapped, const void* completion);


int WINAPI WSAIoctl(Socket s, std::uint32_t code, const void* input, std::uint32_t input_len, void* output, std::uint32_t output_len, std::uint32_t* returned, void* overlapped, const void* completion) {
    if (code == SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER_) {
        wsa_error(WSAEINVAL_);
        return SOCKET_ERROR_;
    }
    int result = HOST("ws2_32.dll", "WSAIoctl", Ioctl)(s, code, input, input_len, output, output_len, returned, overlapped, completion);
    if (result == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER_ && input_len >= 16) {

        Guid guid;
        std::memcpy(&guid, input, 16);
        std::uintptr_t ours;
        if (guid == WSAID_CONNECTEX_) {
            ours = reinterpret_cast<std::uintptr_t>(&connect_ex);
        } else if (guid == WSAID_WSASENDMSG_) {
            ours = reinterpret_cast<std::uintptr_t>(&WSASendMsg);
        } else {
            return result;
        }
        std::memcpy(output, &ours, 8);
    }
    return result;
}


int WINAPI sendto(Socket s, const std::uint8_t* buf, int len, int flags, const std::uint8_t* to, int to_len) {
    if (to && !stream(s) && !reachable("network datagram blocked", to, to_len)) {
        return SOCKET_ERROR_;
    }
    return HOST("ws2_32.dll", "sendto", int(WINAPI*)(Socket, const std::uint8_t*, int, int, const std::uint8_t*, int))(s, buf, len, flags, to, to_len);
}

int WINAPI WSASendTo(Socket s, const void* buffers, std::uint32_t count, std::uint32_t* sent, std::uint32_t flags, const std::uint8_t* to, int to_len, void* overlapped, const void* completion) {
    if (to && !stream(s) && !reachable("network datagram blocked", to, to_len)) {
        return SOCKET_ERROR_;
    }
    return HOST("ws2_32.dll", "WSASendTo", int(WINAPI*)(Socket, const void*, std::uint32_t, std::uint32_t*, std::uint32_t, const std::uint8_t*, int, void*, const void*))(s, buffers, count, sent, flags, to, to_len, overlapped, completion);
}

namespace {

struct MessageName {
    const std::uint8_t* name;
    int len;
};
}   

int WINAPI WSASendMsg(Socket s, const void* message, std::uint32_t flags, std::uint32_t* sent, void* overlapped, const void* completion) {
    if (auto m = static_cast<const MessageName*>(message); m && m->name && !reachable("network datagram blocked", m->name, m->len)) {
        return SOCKET_ERROR_;
    }
    return HOST("ws2_32.dll", "WSASendMsg", int(WINAPI*)(Socket, const void*, std::uint32_t, std::uint32_t*, void*, const void*))(s, message, flags, sent, overlapped, completion);
}


int WINAPI bind(Socket s, const std::uint8_t* name, int len) {
    auto host_bind = HOST("ws2_32.dll", "bind", int(WINAPI*)(Socket, const std::uint8_t*, int));
    auto a = socket_addr(name, len);
    if (a && loopback(a->ip)) {
        return host_bind(s, name, len);
    }
    if (a && unspecified(a->ip)) {
        once("network bind narrowed to loopback", show_addr(*a));

        std::vector<std::uint8_t> local(name, name + len);
        if (a->ip.v6) {
            local[23] = 1;
        } else {
            local[4] = 127;
            local[5] = 0;
            local[6] = 0;
            local[7] = 1;
        }
        return host_bind(s, local.data(), len);
    }
    once("network bind blocked", shown(a));
    wsa_error(WSAEADDRNOTAVAIL_);
    return SOCKET_ERROR_;
}


int WINAPI getaddrinfo(const char* node, const char* service, const void* hints, void** result) {
    if (!resolvable(narrow(node))) {
        return WSAHOST_NOT_FOUND_;
    }
    return HOST("ws2_32.dll", "getaddrinfo", int(WINAPI*)(const char*, const char*, const void*, void**))(node, service, hints, result);
}

int WINAPI GetAddrInfoW(const wchar_t* node, const wchar_t* service, const void* hints, void** result) {
    if (!resolvable(wide(node))) {
        return WSAHOST_NOT_FOUND_;
    }
    return HOST("ws2_32.dll", "GetAddrInfoW", int(WINAPI*)(const wchar_t*, const wchar_t*, const void*, void**))(node, service, hints, result);
}

int WINAPI GetAddrInfoExA(const char* name, const char* service, std::uint32_t name_space, const void* provider, const void* hints, void** result, const void* timeout, void* overlapped, const void* completion, Handle* handle) {
    if (!resolvable(narrow(name))) {
        return WSAHOST_NOT_FOUND_;
    }
    return HOST("ws2_32.dll", "GetAddrInfoExA", int(WINAPI*)(const char*, const char*, std::uint32_t, const void*, const void*, void**, const void*, void*, const void*, Handle*))(name, service, name_space, provider, hints, result, timeout, overlapped, completion, handle);
}

int WINAPI GetAddrInfoExW(const wchar_t* name, const wchar_t* service, std::uint32_t name_space, const void* provider, const void* hints, void** result, const void* timeout, void* overlapped, const void* completion, Handle* handle) {
    if (!resolvable(wide(name))) {
        return WSAHOST_NOT_FOUND_;
    }
    return HOST("ws2_32.dll", "GetAddrInfoExW", int(WINAPI*)(const wchar_t*, const wchar_t*, std::uint32_t, const void*, const void*, void**, const void*, void*, const void*, Handle*))(name, service, name_space, provider, hints, result, timeout, overlapped, completion, handle);
}

void* WINAPI gethostbyname(const char* name) {
    if (!resolvable(narrow(name))) {
        return nullptr;
    }
    return HOST("ws2_32.dll", "gethostbyname", void*(WINAPI*)(const char*))(name);
}

Handle WINAPI WSAAsyncGetHostByName(Handle window, std::uint32_t message, const char* name, char* buf, int buf_len) {
    if (!resolvable(narrow(name))) {
        return nullptr;
    }
    return HOST("ws2_32.dll", "WSAAsyncGetHostByName", Handle(WINAPI*)(Handle, std::uint32_t, const char*, char*, int))(window, message, name, buf, buf_len);
}


int WINAPI WSALookupServiceBeginA(const void*, std::uint32_t, Handle*) {
    once("network name-space query blocked", "any");
    wsa_error(WSASERVICE_NOT_FOUND_);
    return SOCKET_ERROR_;
}


int WINAPI WSALookupServiceBeginW(const void* restrictions, std::uint32_t flags, Handle* lookup) { return WSALookupServiceBeginA(restrictions, flags, lookup); }


void* WINAPI gethostbyaddr(const char* addr, int len, int kind) {
    if (!reverse_resolvable(raw_ip(addr, len, kind))) {
        wsa_error(WSAHOST_NOT_FOUND_);
        return nullptr;
    }
    return HOST("ws2_32.dll", "gethostbyaddr", void*(WINAPI*)(const char*, int, int))(addr, len, kind);
}

Handle WINAPI WSAAsyncGetHostByAddr(Handle window, std::uint32_t message, const char* addr, int len, int kind, char* buf, int buf_len) {
    if (!reverse_resolvable(raw_ip(addr, len, kind))) {
        wsa_error(WSAHOST_NOT_FOUND_);
        return nullptr;
    }
    return HOST("ws2_32.dll", "WSAAsyncGetHostByAddr", Handle(WINAPI*)(Handle, std::uint32_t, const char*, int, int, char*, int))(window, message, addr, len, kind, buf, buf_len);
}

int WINAPI getnameinfo(const std::uint8_t* sa, int sa_len, char* host, std::uint32_t host_len, char* serv, std::uint32_t serv_len, int flags) {
    auto f = name_info_flags(sa, sa_len, host != nullptr, flags);
    if (!f) {
        return WSAHOST_NOT_FOUND_;
    }
    return HOST("ws2_32.dll", "getnameinfo", int(WINAPI*)(const std::uint8_t*, int, char*, std::uint32_t, char*, std::uint32_t, int))(sa, sa_len, host, host_len, serv, serv_len, *f);
}

int WINAPI GetNameInfoW(const std::uint8_t* sa, int sa_len, wchar_t* host, std::uint32_t host_len, wchar_t* serv, std::uint32_t serv_len, int flags) {
    auto f = name_info_flags(sa, sa_len, host != nullptr, flags);
    if (!f) {
        return WSAHOST_NOT_FOUND_;
    }
    return HOST("ws2_32.dll", "GetNameInfoW", int(WINAPI*)(const std::uint8_t*, int, wchar_t*, std::uint32_t, wchar_t*, std::uint32_t, int))(sa, sa_len, host, host_len, serv, serv_len, *f);
}


Handle WINAPI WinHttpConnect(Handle session, const wchar_t* server, std::uint16_t port, std::uint32_t reserved) {
    if (auto name = wide(server); name && !loopback_name(*name)) {
        once("HTTP connection blocked", *name + ":" + std::to_string(port));
        SetLastError(ERROR_WINHTTP_CANNOT_CONNECT_);
        return nullptr;
    }
    return HOST("winhttp.dll", "WinHttpConnect", Handle(WINAPI*)(Handle, const wchar_t*, std::uint16_t, std::uint32_t))(session, server, port, reserved);
}


Handle WINAPI WinHttpOpen(const wchar_t* agent, std::uint32_t access, const wchar_t*, const wchar_t*, std::uint32_t flags) {
    if (access != WINHTTP_ACCESS_TYPE_NO_PROXY_) {
        once("HTTP proxy ignored", "access type " + std::to_string(access));
    }
    return HOST("winhttp.dll", "WinHttpOpen", Handle(WINAPI*)(const wchar_t*, std::uint32_t, const wchar_t*, const wchar_t*, std::uint32_t))(agent, WINHTTP_ACCESS_TYPE_NO_PROXY_, nullptr, nullptr, flags);
}


BOOL WINAPI WinHttpSetOption(Handle handle, std::uint32_t option, const void* buffer, std::uint32_t len) {
    if (option == WINHTTP_OPTION_PROXY_) {
        once("HTTP proxy ignored", "WINHTTP_OPTION_PROXY");
        return 1;
    }
    return HOST("winhttp.dll", "WinHttpSetOption", BOOL(WINAPI*)(Handle, std::uint32_t, const void*, std::uint32_t))(handle, option, buffer, len);
}

namespace {

DWORD no_auto_proxy() {
    once("HTTP proxy auto-detection blocked", "WPAD/PAC");
    return ERROR_WINHTTP_AUTODETECTION_FAILED_;
}
}   

BOOL WINAPI WinHttpDetectAutoProxyConfigUrl(std::uint32_t, wchar_t**) {
    SetLastError(no_auto_proxy());
    return 0;
}

BOOL WINAPI WinHttpGetProxyForUrl(Handle, const wchar_t*, const void*, void*) {
    SetLastError(no_auto_proxy());
    return 0;
}

DWORD WINAPI WinHttpGetProxyForUrlEx(Handle, const wchar_t*, const void*, std::uintptr_t) { return no_auto_proxy(); }

DWORD WINAPI WinHttpGetProxyForUrlEx2(Handle, const wchar_t*, const void*, std::uint32_t, const std::uint8_t*, std::uintptr_t) { return no_auto_proxy(); }

DWORD WINAPI WinHttpGetProxySettingsEx(Handle, int, const void*, std::uintptr_t) { return no_auto_proxy(); }


#define E(name, f) HleExport{name, reinterpret_cast<std::uintptr_t>(&f)}
std::vector<HleExport> exports() {
    return {E("connect", connect),
            E("WSAConnect", WSAConnect),
            E("WSAConnectByNameA", WSAConnectByNameA),
            E("WSAConnectByNameW", WSAConnectByNameW),
            E("WSAConnectByList", WSAConnectByList),
            E("WSAJoinLeaf", WSAJoinLeaf),
            E("WSAIoctl", WSAIoctl),
            E("sendto", sendto),
            E("WSASendTo", WSASendTo),
            E("WSASendMsg", WSASendMsg),
            E("bind", bind),
            E("getaddrinfo", getaddrinfo),
            E("GetAddrInfoW", GetAddrInfoW),
            E("GetAddrInfoExA", GetAddrInfoExA),
            E("GetAddrInfoExW", GetAddrInfoExW),
            E("gethostbyname", gethostbyname),
            E("WSAAsyncGetHostByName", WSAAsyncGetHostByName),
            E("WSALookupServiceBeginA", WSALookupServiceBeginA),
            E("WSALookupServiceBeginW", WSALookupServiceBeginW),
            E("gethostbyaddr", gethostbyaddr),
            E("WSAAsyncGetHostByAddr", WSAAsyncGetHostByAddr),
            E("getnameinfo", getnameinfo),
            E("GetNameInfoW", GetNameInfoW),
            E("WinHttpConnect", WinHttpConnect),
            E("WinHttpOpen", WinHttpOpen),
            E("WinHttpSetOption", WinHttpSetOption),
            E("WinHttpDetectAutoProxyConfigUrl", WinHttpDetectAutoProxyConfigUrl),
            E("WinHttpGetProxyForUrl", WinHttpGetProxyForUrl),
            E("WinHttpGetProxyForUrlEx", WinHttpGetProxyForUrlEx),
            E("WinHttpGetProxyForUrlEx2", WinHttpGetProxyForUrlEx2),
            E("WinHttpGetProxySettingsEx", WinHttpGetProxySettingsEx),
            E("#2", bind),
            E("#4", connect),
            E("#20", sendto),
            E("#51", gethostbyaddr),
            E("#52", gethostbyname),
            E("#102", WSAAsyncGetHostByAddr),
            E("#103", WSAAsyncGetHostByName)};
}
#undef E

}   
