#pragma once

/// SocketMixin — full systemd socket activation
///
/// Responsibilities:
///   phase::SocketBind   — parse .socket units, create/bind all fds
///   phase::UnitExecute  — for on-demand (Accept=no): pass fds to paired service
///                         for inetd-style (Accept=yes): accept loop in main thread
///   Runtime             — accept_ready(fd) called from poll loop for Accept=yes sockets
///
/// Protocol (SD_LISTEN_FDS):
///   LISTEN_PID=<child_pid>        set just before exec in child
///   LISTEN_FDS=<n>                number of inherited fds
///   LISTEN_FDNAMES=name0:name1:…  colon-separated fd names
///   fds start at SD_LISTEN_FDS_START (3)
///
/// All fd lifecycle: fds are created in SocketMixin, set CLOEXEC=off before
/// exec of the target service, then restored to CLOEXEC in the parent.

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/journal.hpp"
#include "trivialInit/systemd/socket_unit.hpp"
#include "trivialInit/systemd/unit_file.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <mqueue.h>
#include <linux/netlink.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cassert>

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <charconv>

namespace tinit {

static constexpr int SD_LISTEN_FDS_START = 3;

namespace detail {

/// Parse "host:port" or ":port" or "port" for TCP/UDP.
/// Returns {host, port} — host is empty string for wildcard.
inline std::pair<std::string,int> parse_inet_addr(std::string_view addr) {
    // IPv6 literal: [::1]:22
    if (!addr.empty() && addr[0] == '[') {
        auto close = addr.find(']');
        if (close == std::string_view::npos) return {"", 0};
        std::string host(addr.substr(1, close - 1));
        int port = 0;
        if (close + 2 < addr.size()) {
            std::from_chars(addr.data() + close + 2, addr.data() + addr.size(), port);
        }
        return {host, port};
    }
    // host:port or :port or port
    auto colon = addr.rfind(':');
    if (colon == std::string_view::npos) {
        // bare port number
        int port = 0;
        std::from_chars(addr.data(), addr.data() + addr.size(), port);
        return {"", port};
    }
    std::string host(addr.substr(0, colon));
    int port = 0;
    std::from_chars(addr.data() + colon + 1, addr.data() + addr.size(), port);
    return {host, port};
}

/// Parse netlink family string: "KOBJECT_UEVENT", "ROUTE", "AUDIT", etc.
/// Returns netlink family number.
inline int parse_netlink_family(std::string_view s) {
    struct { const char* name; int id; } table[] = {
        {"ROUTE",          NETLINK_ROUTE},
        {"KOBJECT_UEVENT", NETLINK_KOBJECT_UEVENT},
        {"AUDIT",          NETLINK_AUDIT},
        {"SCSITRANSPORT",  NETLINK_SCSITRANSPORT},
        {"FIREWALL",       NETLINK_FIREWALL},
        {"NFLOG",          NETLINK_NFLOG},
        {"XFRM",           NETLINK_XFRM},
        {"SELINUX",        NETLINK_SELINUX},
        {"ISCSI",          NETLINK_ISCSI},
        {"CRYPTO",         NETLINK_CRYPTO},
    };
    for (auto& e : table)
        if (s == e.name) return e.id;
    // numeric fallback
    int id = 0;
    std::from_chars(s.data(), s.data() + s.size(), id);
    return id;
}

inline int parse_octal(std::string_view sv, int def) {
    int v = 0;
    for (char c : sv) {
        if (c < '0' || c > '7') return def;
        v = v * 8 + (c - '0');
    }
    return v ? v : def;
}

inline bool parse_bool(std::string_view sv, bool def = false) {
    if (sv == "yes" || sv == "true" || sv == "1" || sv == "on") return true;
    if (sv == "no"  || sv == "false"|| sv == "0" || sv == "off") return false;
    return def;
}

inline int parse_int(std::string_view sv, int def = 0) {
    int v = def;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

inline void set_cloexec(int fd, bool on) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return;
    if (on)  flags |=  FD_CLOEXEC;
    else     flags &= ~FD_CLOEXEC;
    fcntl(fd, F_SETFD, flags);
}

inline void set_nonblock(int fd, bool on) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return;
    if (on)  flags |=  O_NONBLOCK;
    else     flags &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

} // namespace detail

template <typename Derived>
struct SocketMixin : MixinBase<Derived, SocketMixin<Derived>> {

    // All parsed .socket units
    std::unordered_map<std::string, SocketUnit> socket_units_;

    // fd → socket unit name (for epoll dispatch)
    std::unordered_map<int, std::string> fd_to_socket_;

    // ----------------------------------------------------------------
    // Phase: SocketBind
    // Called after UnitParse, before UnitExecute.
    // Parses .socket sections from units_, creates + binds all fds.
    // ----------------------------------------------------------------
    int execute(phase::SocketBind) {
        auto& s = this->self();
        s.log(LogLevel::Info, "socket", "Binding socket units");

        for (const auto& [name, unit] : s.all_units()) {
            if (unit.type != UnitType::Socket) continue;
            auto sock = parse_socket_unit(unit);
            if (bind_socket_unit(sock) != 0) {
                s.log_fmt(LogLevel::Error, "socket", "Failed to bind {}", name);
                // non-fatal: continue with others
            }
            socket_units_[name] = std::move(sock);
        }

        s.log_fmt(LogLevel::Info, "socket",
            "Bound {} socket unit(s)", socket_units_.size());
        return 0;
    }

    // ----------------------------------------------------------------
    // Called by UnitExecMixin::start_service when the service being
    // launched has associated socket fds.  Returns the number of fds
    // passed, or 0 if none.
    // ----------------------------------------------------------------
    int pass_fds_to_service(const std::string& service_name, pid_t child_pid) {
        auto& s = this->self();

        // Collect all ListenEntries whose socket unit pairs with service_name
        std::vector<const ListenEntry*> entries;
        std::vector<std::string>        names;

        for (auto& [sname, sock] : socket_units_) {
            if (sock.paired_service() != service_name) continue;
            for (auto& le : sock.listen) {
                if (le.fd < 0) continue;
                entries.push_back(&le);
                names.push_back(le.name.empty() ? sname : le.name);
            }
        }

        if (entries.empty()) return 0;

        // Rearrange: dup2 each fd into consecutive slots starting at
        // SD_LISTEN_FDS_START.  We do this in the child (called post-fork,
        // pre-exec) via the spawn_with_socket_fds helper below.
        // Here in the parent we just set env vars and clear CLOEXEC.
        // (The actual dup2 sequence happens inside spawn_with_socket_fds.)

        for (auto* le : entries) detail::set_cloexec(le->fd, false);

        // Set env vars for child (written to a staging buffer, applied in child)
        // We store them and pass into spawn.
        s.log_fmt(LogLevel::Debug, "socket",
            "Passing {} fd(s) to {}", entries.size(), service_name);
        return static_cast<int>(entries.size());
    }

    // ----------------------------------------------------------------
    // Build the env-var additions needed for socket activation.
    // Called pre-fork by UnitExecMixin.
    // ----------------------------------------------------------------
    struct SocketEnv {
        std::vector<int>         fds;    // in SD_LISTEN_FDS_START+i order
        std::vector<std::string> names;  // parallel
    };

    std::optional<SocketEnv> collect_socket_env(const std::string& service_name) {
        SocketEnv env;
        for (auto& [sname, sock] : socket_units_) {
            if (sock.paired_service() != service_name) continue;
            for (auto& le : sock.listen) {
                if (le.fd < 0) continue;
                env.fds.push_back(le.fd);
                env.names.push_back(le.name.empty() ? sname : le.name);
            }
        }
        if (env.fds.empty()) return std::nullopt;
        return env;
    }

    // ----------------------------------------------------------------
    // Register all socket fds with epoll (called after SocketBind,
    // before the main loop) for Accept=yes sockets.
    // ----------------------------------------------------------------
    void register_accept_sockets(int epfd) {
        for (auto& [sname, sock] : socket_units_) {
            if (!sock.accept) continue;
            for (auto& le : sock.listen) {
                if (le.fd < 0) continue;
                detail::set_nonblock(le.fd, true);
                epoll_event ev{};
                ev.events   = EPOLLIN;
                ev.data.fd  = le.fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, le.fd, &ev);
                fd_to_socket_[le.fd] = sname;
            }
        }
    }

    // ----------------------------------------------------------------
    // Called from the main poll loop when a listening fd becomes
    // readable (Accept=yes path).  Accepts the connection and spawns
    // the paired service with the accepted fd at SD_LISTEN_FDS_START.
    // ----------------------------------------------------------------
    void accept_ready(int listen_fd) {
        auto& s = this->self();

        auto it = fd_to_socket_.find(listen_fd);
        if (it == fd_to_socket_.end()) return;
        auto& sock = socket_units_[it->second];

        // MaxConnections enforcement
        if (sock.max_conn > 0 && sock.active_connections >= sock.max_conn) {
            s.log_fmt(LogLevel::Warning, "socket",
                "{}: MaxConnections={} reached, dropping", sock.name, sock.max_conn);
            // Just drain the accept queue entry without spawning
            int conn = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (conn >= 0) ::close(conn);
            return;
        }

        int conn = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                s.log_fmt(LogLevel::Warning, "socket",
                    "accept on {} failed: {}", sock.name, strerror(errno));
            return;
        }

        sock.active_connections++;

        const std::string& svc = sock.paired_service();
        const auto* unit = s.find_unit(svc);
        if (!unit || unit->exec_start.empty()) {
            s.log_fmt(LogLevel::Warning, "socket",
                "{}: no paired service {} found", sock.name, svc);
            ::close(conn);
            sock.active_connections--;
            return;
        }

        // Pass single accepted fd as the activation fd
        SocketEnv env;
        env.fds.push_back(conn);
        env.names.push_back(sock.name);

        auto args = detail_split_exec(unit->exec_start);
        if (args.empty()) { ::close(conn); sock.active_connections--; return; }

        std::string exec_path = args[0];
        args.erase(args.begin());

        s.log_fmt(LogLevel::Info, "socket",
            "Accept=yes: spawning {} for {}", svc, sock.name);

        pid_t pid = spawn_with_socket_fds(svc, exec_path, args, env, false);
        ::close(conn); // parent closes its copy after fork

        if (pid > 0) {
            // Decrement active_connections when this child exits
            s.on_child_exit(pid, [this, &sock](int) {
                sock.active_connections--;
            });
        } else {
            sock.active_connections--;
        }
    }

    // ----------------------------------------------------------------
    // Spawn a service with socket fds dup2'd into place.
    // This replaces ProcessMixin::spawn for socket-activated services.
    // ----------------------------------------------------------------
    pid_t spawn_with_socket_fds(
            const std::string& unit_name,
            const std::string& exec_path,
            const std::vector<std::string>& args,
            const SocketEnv& env,
            bool restart)
    {
        auto& s = this->self();

        // Temporarily clear CLOEXEC on all activation fds
        for (int fd : env.fds) detail::set_cloexec(fd, false);

        pid_t pid = ::fork();
        if (pid < 0) {
            s.log_fmt(LogLevel::Error, "socket",
                "fork for {} failed: {}", unit_name, strerror(errno));
            for (int fd : env.fds) detail::set_cloexec(fd, true);
            return -1;
        }

        if (pid == 0) {
            // ---- CHILD ----

            // Reset signal mask
            sigset_t empty; sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, nullptr);
            setsid();

            // Dup fds into consecutive slots starting at SD_LISTEN_FDS_START
            // First, push them out of the target range if they happen to collide.
            std::vector<int> working(env.fds);
            for (int i = 0; i < static_cast<int>(working.size()); ++i) {
                int target = SD_LISTEN_FDS_START + i;
                if (working[i] == target) continue;

                // Make sure target slot is free
                ::close(target);  // ignore error if not open

                if (::dup2(working[i], target) < 0) _exit(125);

                // Update any later entries that referenced the old fd
                for (int j = i + 1; j < static_cast<int>(working.size()); ++j)
                    if (working[j] == working[i]) working[j] = target;

                // Close the original (unless it's one of the later targets)
                bool keep = false;
                for (int j = i + 1; j < static_cast<int>(working.size()); ++j)
                    if (working[j] == working[i]) { keep = true; break; }
                if (!keep) ::close(working[i]);
            }

            // Set CLOEXEC on all fds above SD_LISTEN_FDS_START + n (tidy up)
            // (not strictly required; kernel closes CLOEXEC on exec)

            // Build env strings
            int n = static_cast<int>(env.fds.size());
            std::string listen_pid   = "LISTEN_PID=" + std::to_string(getpid());
            std::string listen_fds   = "LISTEN_FDS=" + std::to_string(n);
            std::string listen_names = "LISTEN_FDNAMES=";
            for (int i = 0; i < n; ++i) {
                if (i) listen_names += ':';
                listen_names += env.names[i];
            }
            putenv(listen_pid.data());
            putenv(listen_fds.data());
            putenv(listen_names.data());

            // Build argv
            std::vector<const char*> argv;
            argv.push_back(exec_path.c_str());
            for (auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);

            execvp(exec_path.c_str(), const_cast<char**>(argv.data()));
            _exit(127);
        }

        // ---- PARENT ----
        // Restore CLOEXEC (we keep the fds open for future activations)
        for (int fd : env.fds) detail::set_cloexec(fd, true);

        s.log_fmt(LogLevel::Info, "socket",
            "Socket-activated {} (PID {}, {} fd(s))", unit_name, pid, env.fds.size());

        // Register with ProcessMixin
        s.track_process(pid, unit_name, restart);
        return pid;
    }

    // ----------------------------------------------------------------
    // Close all socket fds during shutdown
    // ----------------------------------------------------------------
    void close_all_sockets() {
        for (auto& [name, sock] : socket_units_) {
            for (auto& le : sock.listen) {
                if (le.fd >= 0) {
                    ::close(le.fd);
                    // Remove Unix socket files
                    if (!le.address.empty() && le.address[0] == '/')
                        ::unlink(le.address.c_str());
                    le.fd = -1;
                }
            }
        }
    }

    const std::unordered_map<std::string, SocketUnit>& all_socket_units() const {
        return socket_units_;
    }

    // ================================================================
private:
    // ================================================================

    /// Parse a UnitFile of type Socket into a SocketUnit
    SocketUnit parse_socket_unit(const UnitFile& unit) {
        SocketUnit sock;
        sock.name = unit.name;
        sock.path = unit.path;
        sock.description = unit.description;
        sock.wants       = unit.wants;
        sock.requires_   = unit.requires_;
        sock.after       = unit.after;
        sock.before      = unit.before;
        sock.wanted_by   = unit.wanted_by;
        sock.required_by = unit.required_by;

        auto it = unit.raw_sections.find("Socket");
        if (it == unit.raw_sections.end()) return sock;
        const auto& sec = it->second;

        auto get = [&](const char* k) -> std::string_view {
            auto jt = sec.find(k);
            return jt != sec.end() ? std::string_view(jt->second) : std::string_view{};
        };

        // --- Listen* directives ---
        // Each may appear multiple times; we stored them space-concatenated,
        // so split on space and handle each token.
        auto add_listen = [&](std::string_view raw, ListenType lt) {
            if (raw.empty()) return;
            // May be space-separated from multiple directive lines
            std::string_view delims = " \t";
            size_t pos = 0;
            while (pos < raw.size()) {
                auto start = raw.find_first_not_of(delims, pos);
                if (start == std::string_view::npos) break;
                auto end   = raw.find_first_of(delims, start);
                if (end   == std::string_view::npos) end = raw.size();
                ListenEntry le;
                le.type    = lt;
                le.address = std::string(raw.substr(start, end - start));
                sock.listen.push_back(std::move(le));
                pos = end;
            }
        };

        add_listen(get("ListenStream"),           ListenType::Stream);
        add_listen(get("ListenDatagram"),          ListenType::Datagram);
        add_listen(get("ListenSequentialPacket"),  ListenType::SequentialPacket);
        add_listen(get("ListenFIFO"),              ListenType::FIFO);
        add_listen(get("ListenSpecial"),           ListenType::Special);
        add_listen(get("ListenNetlink"),           ListenType::Netlink);
        add_listen(get("ListenMessageQueue"),      ListenType::MessageQueue);

        // --- Options ---
        if (auto v = get("Accept");       !v.empty()) sock.accept      = detail::parse_bool(v);
        if (auto v = get("Backlog");      !v.empty()) sock.backlog     = detail::parse_int(v, 128);
        if (auto v = get("MaxConnections");!v.empty()) sock.max_conn   = detail::parse_int(v);
        if (auto v = get("MaxConnectionsPerSource");!v.empty())
            sock.max_conn_per_source = detail::parse_int(v);
        if (auto v = get("SocketMode");   !v.empty()) sock.socket_mode = detail::parse_octal(v, 0666);
        if (auto v = get("DirectoryMode");!v.empty()) sock.directory_mode = detail::parse_octal(v, 0755);
        if (auto v = get("ReusePort");    !v.empty()) sock.reuse_port  = detail::parse_bool(v);
        if (auto v = get("FreeBind");     !v.empty()) sock.free_bind   = detail::parse_bool(v);
        if (auto v = get("Transparent");  !v.empty()) sock.transparent = detail::parse_bool(v);
        if (auto v = get("Broadcast");    !v.empty()) sock.broadcast   = detail::parse_bool(v);
        if (auto v = get("PassCredentials");!v.empty()) sock.pass_credentials = detail::parse_bool(v);
        if (auto v = get("PassSecurity"); !v.empty()) sock.pass_security = detail::parse_bool(v);
        if (auto v = get("KeepAlive");    !v.empty()) sock.keep_alive  = detail::parse_bool(v);
        if (auto v = get("NoDelay");      !v.empty()) sock.no_delay    = detail::parse_bool(v);
        if (auto v = get("Priority");     !v.empty()) sock.priority    = detail::parse_int(v);
        if (auto v = get("ReceiveBuffer");!v.empty()) sock.receive_buffer = detail::parse_int(v);
        if (auto v = get("SendBuffer");   !v.empty()) sock.send_buffer = detail::parse_int(v);
        if (auto v = get("IPTOS");        !v.empty()) sock.iptos       = detail::parse_int(v, -1);
        if (auto v = get("IPTTL");        !v.empty()) sock.ipttl       = detail::parse_int(v, -1);
        if (auto v = get("Mark");         !v.empty()) sock.mark        = detail::parse_int(v, -1);
        if (auto v = get("SocketUser");   !v.empty()) sock.socket_user = std::string(v);
        if (auto v = get("SocketGroup");  !v.empty()) sock.socket_group= std::string(v);
        if (auto v = get("Service");      !v.empty()) sock.service     = std::string(v);

        if (auto v = get("BindIPv6Only"); !v.empty()) {
            if (v == "ipv6-only") sock.ipv6only = BindIPv6Only::Ipv6Only;
            else if (v == "both") sock.ipv6only = BindIPv6Only::Both;
            else sock.ipv6only = BindIPv6Only::Default;
        }

        return sock;
    }

    /// Create, configure, and bind/listen on all fds for a SocketUnit
    int bind_socket_unit(SocketUnit& sock) {
        auto& s = this->self();
        int failures = 0;

        for (auto& le : sock.listen) {
            int fd = create_and_bind(sock, le);
            if (fd < 0) {
                s.log_fmt(LogLevel::Error, "socket",
                    "{}: failed to bind {}: {}", sock.name, le.address, strerror(errno));
                ++failures;
                continue;
            }
            // All fds are CLOEXEC by default; we clear it only during exec
            detail::set_cloexec(fd, true);
            le.fd = fd;
            s.log_fmt(LogLevel::Info, "socket",
                "{}: bound fd {} → {}", sock.name, fd, le.address);
        }

        return failures ? -1 : 0;
    }

    int create_and_bind(const SocketUnit& sock, const ListenEntry& le) {
        switch (le.type) {
        case ListenType::Stream:
        case ListenType::Datagram:
        case ListenType::SequentialPacket:
            return bind_network_or_unix(sock, le);

        case ListenType::FIFO:
            return create_fifo(sock, le);

        case ListenType::Special:
            return open_special(le);

        case ListenType::Netlink:
            return bind_netlink(sock, le);

        case ListenType::MessageQueue:
            return open_mqueue(sock, le);
        }
        return -1;
    }

    int bind_network_or_unix(const SocketUnit& sock, const ListenEntry& le) {
        const std::string& addr = le.address;

        if (!addr.empty() && (addr[0] == '/' || addr[0] == '@')) {
            // Unix domain socket
            return bind_unix(sock, le);
        }
        // Inet (IPv4 or IPv6)
        return bind_inet(sock, le);
    }

    int bind_unix(const SocketUnit& sock, const ListenEntry& le) {
        int type = SOCK_STREAM;
        if (le.type == ListenType::Datagram)         type = SOCK_DGRAM;
        if (le.type == ListenType::SequentialPacket) type = SOCK_SEQPACKET;

        int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;

        apply_socket_opts(fd, sock, le);

        const std::string& path = le.address;
        bool abstract = (path[0] == '@');

        sockaddr_un sun{};
        sun.sun_family = AF_UNIX;

        if (abstract) {
            // Abstract namespace: leading NUL byte
            sun.sun_path[0] = '\0';
            strncpy(sun.sun_path + 1, path.c_str() + 1,
                    sizeof(sun.sun_path) - 2);
            socklen_t len = offsetof(sockaddr_un, sun_path)
                          + 1 + strlen(path.c_str() + 1);
            if (::bind(fd, reinterpret_cast<sockaddr*>(&sun), len) < 0) {
                ::close(fd); return -1;
            }
        } else {
            // Filesystem socket — remove stale socket first
            ::unlink(path.c_str());
            // Ensure parent dir exists
            auto slash = path.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = path.substr(0, slash);
                ::mkdir(dir.c_str(), sock.directory_mode);
            }
            strncpy(sun.sun_path, path.c_str(), sizeof(sun.sun_path) - 1);
            if (::bind(fd, reinterpret_cast<sockaddr*>(&sun),
                       sizeof(sun)) < 0) {
                ::close(fd); return -1;
            }
            ::chmod(path.c_str(), sock.socket_mode);
        }

        if (type != SOCK_DGRAM) {
            if (::listen(fd, sock.backlog) < 0) {
                ::close(fd); return -1;
            }
        }
        return fd;
    }

    int bind_inet(const SocketUnit& sock, const ListenEntry& le) {
        auto [host, port] = detail::parse_inet_addr(le.address);
        if (port == 0) return -1;

        int type = SOCK_STREAM;
        if (le.type == ListenType::Datagram)         type = SOCK_DGRAM;
        if (le.type == ListenType::SequentialPacket) type = SOCK_SEQPACKET;

        // Try IPv6 first (dual-stack or v6-only), fall back to IPv4
        int fd = -1;
        bool use_ipv6 = host.empty() || host.find(':') != std::string::npos;

        if (use_ipv6) {
            fd = ::socket(AF_INET6, type | SOCK_CLOEXEC, 0);
            if (fd >= 0) {
                int v6only = (sock.ipv6only == BindIPv6Only::Ipv6Only) ? 1 : 0;
                ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            }
        }
        if (fd < 0) {
            fd = ::socket(AF_INET, type | SOCK_CLOEXEC, 0);
            if (fd < 0) return -1;
            use_ipv6 = false;
        }

        apply_socket_opts(fd, sock, le);

        if (use_ipv6) {
            sockaddr_in6 sa{};
            sa.sin6_family = AF_INET6;
            sa.sin6_port   = htons(static_cast<uint16_t>(port));
            if (host.empty())
                sa.sin6_addr = IN6ADDR_ANY_INIT;
            else
                ::inet_pton(AF_INET6, host.c_str(), &sa.sin6_addr);
            if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
                ::close(fd); return -1;
            }
        } else {
            sockaddr_in sa{};
            sa.sin_family      = AF_INET;
            sa.sin_port        = htons(static_cast<uint16_t>(port));
            sa.sin_addr.s_addr = host.empty() ? INADDR_ANY
                : ::inet_addr(host.c_str());
            if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
                ::close(fd); return -1;
            }
        }

        if (type != SOCK_DGRAM) {
            if (::listen(fd, sock.backlog) < 0) {
                ::close(fd); return -1;
            }
        }
        return fd;
    }

    int bind_netlink(const SocketUnit& sock, const ListenEntry& le) {
        // address format: "KOBJECT_UEVENT" or "ROUTE:12345" (family:groups)
        std::string_view addr = le.address;
        int family = NETLINK_KOBJECT_UEVENT;
        uint32_t groups = 0;

        auto colon = addr.find(':');
        if (colon != std::string_view::npos) {
            family = detail::parse_netlink_family(addr.substr(0, colon));
            std::from_chars(addr.data() + colon + 1, addr.data() + addr.size(),
                            groups);
        } else {
            family = detail::parse_netlink_family(addr);
        }

        int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, family);
        if (fd < 0) return -1;

        sockaddr_nl sa{};
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = groups;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            ::close(fd); return -1;
        }
        return fd;
    }

    int create_fifo(const SocketUnit& sock, const ListenEntry& le) {
        const char* path = le.address.c_str();
        ::unlink(path);
        if (::mkfifo(path, sock.socket_mode) < 0) return -1;
        // Open non-blocking so we don't block on open()
        int fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        return fd;
    }

    int open_special(const ListenEntry& le) {
        // Open a character or block device node
        int fd = ::open(le.address.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        return fd;
    }

    int open_mqueue(const SocketUnit& sock, const ListenEntry& le) {
        // address is the queue name, e.g. "/myqueue"
        struct mq_attr attr{};
        attr.mq_flags   = 0;
        attr.mq_maxmsg  = 10;
        attr.mq_msgsize = 8192;
        mqd_t mqd = ::mq_open(le.address.c_str(),
                               O_RDWR | O_CREAT | O_NONBLOCK,
                               sock.socket_mode, &attr);
        // mq_open returns mqd_t which IS an int on Linux
        return static_cast<int>(mqd);
    }

    // Apply all socket-level options
    void apply_socket_opts(int fd, const SocketUnit& sock,
                           const ListenEntry& /*le*/) {
        int on = 1;
        // SO_REUSEADDR always (mirrors systemd default)
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        if (sock.reuse_port)
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        if (sock.free_bind)
            ::setsockopt(fd, IPPROTO_IP, IP_FREEBIND, &on, sizeof(on));
        if (sock.transparent)
            ::setsockopt(fd, IPPROTO_IP, IP_TRANSPARENT, &on, sizeof(on));
        if (sock.broadcast)
            ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        if (sock.pass_credentials)
            ::setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));
        if (sock.keep_alive)
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        if (sock.no_delay)
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        if (sock.priority >= 0)
            ::setsockopt(fd, SOL_SOCKET, SO_PRIORITY,
                         &sock.priority, sizeof(sock.priority));
        if (sock.receive_buffer > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                         &sock.receive_buffer, sizeof(sock.receive_buffer));
        if (sock.send_buffer > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                         &sock.send_buffer, sizeof(sock.send_buffer));
        if (sock.mark >= 0)
            ::setsockopt(fd, SOL_SOCKET, SO_MARK,
                         &sock.mark, sizeof(sock.mark));
        if (sock.iptos >= 0)
            ::setsockopt(fd, IPPROTO_IP, IP_TOS,
                         &sock.iptos, sizeof(sock.iptos));
        if (sock.ipttl >= 0)
            ::setsockopt(fd, IPPROTO_IP, IP_TTL,
                         &sock.ipttl, sizeof(sock.ipttl));

#ifdef SO_PASSSEC
        if (sock.pass_security)
            ::setsockopt(fd, SOL_SOCKET, SO_PASSSEC, &on, sizeof(on));
#endif
    }

    /// Minimal argv splitter (used in accept_ready)
    static std::vector<std::string> detail_split_exec(const std::string& s) {
        std::vector<std::string> result;
        std::string token;
        bool in_quote = false;
        char quote_char = 0;
        for (char c : s) {
            if (in_quote) {
                if (c == quote_char) in_quote = false;
                else token += c;
            } else if (c == '"' || c == '\'') {
                in_quote = true; quote_char = c;
            } else if (c == ' ' || c == '\t') {
                if (!token.empty()) { result.push_back(token); token.clear(); }
            } else {
                token += c;
            }
        }
        if (!token.empty()) result.push_back(token);
        return result;
    }
};

} // namespace tinit