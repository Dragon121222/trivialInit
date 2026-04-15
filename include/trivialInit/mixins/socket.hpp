#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/journal.hpp"
#include "trivialInit/systemd/socket_unit.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include "trivialInit/systemd/specifiers.hpp"
#include "trivialInit/mixins/process.hpp"  // for setup_child_context

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
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <charconv>

namespace tinit {

static constexpr int SD_LISTEN_FDS_START = 3;

namespace detail {

inline std::pair<std::string,int> parse_inet_addr(std::string_view addr) {
    if (!addr.empty() && addr[0] == '[') {
        auto close = addr.find(']');
        if (close == std::string_view::npos) return {"", 0};
        std::string host(addr.substr(1, close - 1));
        int port = 0;
        if (close + 2 < addr.size())
            std::from_chars(addr.data() + close + 2, addr.data() + addr.size(), port);
        return {host, port};
    }
    auto colon = addr.rfind(':');
    if (colon == std::string_view::npos) {
        int port = 0; std::from_chars(addr.data(), addr.data() + addr.size(), port);
        return {"", port};
    }
    std::string host(addr.substr(0, colon));
    int port = 0;
    std::from_chars(addr.data() + colon + 1, addr.data() + addr.size(), port);
    return {host, port};
}

inline int parse_netlink_family(std::string_view s) {
    struct { const char* name; int id; } table[] = {
        {"ROUTE",NETLINK_ROUTE},{"KOBJECT_UEVENT",NETLINK_KOBJECT_UEVENT},
        {"AUDIT",NETLINK_AUDIT},{"SCSITRANSPORT",NETLINK_SCSITRANSPORT},
        {"FIREWALL",NETLINK_FIREWALL},{"NFLOG",NETLINK_NFLOG},
        {"XFRM",NETLINK_XFRM},{"SELINUX",NETLINK_SELINUX},
        {"ISCSI",NETLINK_ISCSI},{"CRYPTO",NETLINK_CRYPTO},
    };
    for (auto& e : table) if (s == e.name) return e.id;
    int id = 0; std::from_chars(s.data(), s.data() + s.size(), id); return id;
}

inline int  parse_octal(std::string_view sv, int def) {
    int v=0; for(char c:sv){if(c<'0'||c>'7')return def;v=v*8+(c-'0');} return v?v:def;
}
inline bool parse_bool(std::string_view sv, bool def=false) {
    if(sv=="yes"||sv=="true"||sv=="1"||sv=="on")return true;
    if(sv=="no"||sv=="false"||sv=="0"||sv=="off")return false; return def;
}
inline int  parse_int(std::string_view sv, int def=0) {
    int v=def; std::from_chars(sv.data(),sv.data()+sv.size(),v); return v;
}
inline void set_cloexec(int fd, bool on) {
    int f=fcntl(fd,F_GETFD); if(f<0)return;
    fcntl(fd,F_SETFD,on?(f|FD_CLOEXEC):(f&~FD_CLOEXEC));
}
inline void set_nonblock(int fd, bool on) {
    int f=fcntl(fd,F_GETFL); if(f<0)return;
    fcntl(fd,F_SETFL,on?(f|O_NONBLOCK):(f&~O_NONBLOCK));
}

} // namespace detail

template <typename Derived>
struct SocketMixin : MixinBase<Derived, SocketMixin<Derived>> {

    std::unordered_map<std::string, SocketUnit> socket_units_;
    std::unordered_map<int, std::string>         fd_to_socket_;
    SpecifierResolver                            spec_resolver_;

    int execute(phase::SocketBind) {
        auto& s = this->self();
        s.log(LogLevel::Info, "socket", "Binding socket units");
        int bound = 0;

        for (const auto& [name, unit] : s.all_units()) {
            if (unit.type != UnitType::Socket) continue;

            // Skip bare template socket units — foo@.socket has no instance
            if (unit.is_bare_template()) {
                s.log_fmt(LogLevel::Debug, "socket",
                    "{}: skipping bare template", name);
                continue;
            }

            auto sock = parse_socket_unit(unit);
            expand_specifiers(sock);

            // Skip if all addresses still contain unexpanded specifiers
            bool has_bindable = false;
            for (auto& le : sock.listen)
                if (!le.address.empty() && le.address.find('%') == std::string::npos)
                    has_bindable = true;

            if (!has_bindable && !sock.listen.empty()) {
                s.log_fmt(LogLevel::Debug, "socket",
                    "{}: skipping — no bindable addresses after expansion", name);
                socket_units_[name] = std::move(sock);
                continue;
            }

            bind_socket_unit(sock);
            socket_units_[name] = std::move(sock);
            ++bound;
        }

        s.log_fmt(LogLevel::Info, "socket", "Bound {} socket unit(s)", bound);
        return 0;
    }

    struct SocketEnv {
        std::vector<int>         fds;
        std::vector<std::string> names;
    };

    std::optional<SocketEnv> collect_socket_env(const std::string& service_name) {
        SocketEnv env;
        for (auto& [sname, sock] : socket_units_) {
            if (!pairs_with(sock, service_name)) continue;
            for (auto& le : sock.listen) {
                if (le.fd < 0) continue;
                env.fds.push_back(le.fd);
                env.names.push_back(le.name.empty() ? sname : le.name);
            }
        }
        if (env.fds.empty()) return std::nullopt;
        return env;
    }

    void register_accept_sockets(int epfd) {
        for (auto& [sname, sock] : socket_units_) {
            if (!sock.accept) continue;
            for (auto& le : sock.listen) {
                if (le.fd < 0) continue;
                detail::set_nonblock(le.fd, true);
                epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = le.fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, le.fd, &ev);
                fd_to_socket_[le.fd] = sname;
            }
        }
    }

    void accept_ready(int listen_fd) {
        auto& s = this->self();
        auto it = fd_to_socket_.find(listen_fd);
        if (it == fd_to_socket_.end()) return;
        auto& sock = socket_units_[it->second];

        if (sock.max_conn > 0 && sock.active_connections >= sock.max_conn) {
            int c = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c >= 0) ::close(c);
            return;
        }
        int conn = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) return;
        sock.active_connections++;

        std::string svc = resolve_pair(sock);
        const auto* unit = s.find_unit(svc);
        if (!unit || unit->exec_start.empty()) { ::close(conn); sock.active_connections--; return; }

        SocketEnv env; env.fds.push_back(conn); env.names.push_back(sock.name);
        auto args = split_exec(unit->exec_start);
        if (args.empty()) { ::close(conn); sock.active_connections--; return; }
        std::string exec_path = args[0]; args.erase(args.begin());

        pid_t pid = spawn_with_socket_fds(svc, exec_path, args, env, false, unit);
        ::close(conn);
        if (pid > 0) s.on_child_exit(pid, [this,&sock](int){ sock.active_connections--; });
        else sock.active_connections--;
    }

    pid_t spawn_with_socket_fds(const std::string& unit_name,
                                const std::string& exec_path,
                                const std::vector<std::string>& args,
                                const SocketEnv& env,
                                bool restart,
                                const UnitFile* unit_ctx = nullptr)
    {
        auto& s = this->self();
        for (int fd : env.fds) detail::set_cloexec(fd, false);

        pid_t pid = ::fork();
        if (pid < 0) { for (int fd : env.fds) detail::set_cloexec(fd, true); return -1; }

        if (pid == 0) {
            sigset_t empty; sigemptyset(&empty); sigprocmask(SIG_SETMASK, &empty, nullptr);
            setsid();

            std::vector<int> w(env.fds);
            for (int i = 0; i < (int)w.size(); ++i) {
                int tgt = SD_LISTEN_FDS_START + i;
                if (w[i] == tgt) continue;
                ::close(tgt);
                if (::dup2(w[i], tgt) < 0) _exit(125);
                for (int j=i+1;j<(int)w.size();++j) if(w[j]==w[i]) w[j]=tgt;
                bool keep=false;
                for (int j=i+1;j<(int)w.size();++j) if(w[j]==w[i]){keep=true;break;}
                if (!keep) ::close(w[i]);
            }
            int n = (int)env.fds.size();
            std::string lp="LISTEN_PID="+std::to_string(getpid());
            std::string lf="LISTEN_FDS="+std::to_string(n);
            std::string ln="LISTEN_FDNAMES=";
            for (int i=0;i<n;++i){if(i)ln+=':';ln+=env.names[i];}
            putenv(lp.data()); putenv(lf.data()); putenv(ln.data());
            // Apply credentials AFTER fd rearrangement — setuid must not close fds
            if (unit_ctx) setup_child_context(*unit_ctx);
            std::vector<const char*> argv;
            argv.push_back(exec_path.c_str());
            for (auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);
            execvp(exec_path.c_str(), const_cast<char**>(argv.data()));
            _exit(127);
        }

        for (int fd : env.fds) detail::set_cloexec(fd, true);
        s.log_fmt(LogLevel::Info, "socket",
            "Socket-activated {} (PID {}, {} fd(s))", unit_name, pid, env.fds.size());
        s.track_process(pid, unit_name, restart);
        return pid;
    }

    void close_all_sockets() {
        for (auto& [name, sock] : socket_units_) {
            for (auto& le : sock.listen) {
                if (le.fd >= 0) {
                    ::close(le.fd);
                    if (!le.address.empty() && le.address[0]=='/')
                        ::unlink(le.address.c_str());
                    le.fd = -1;
                }
            }
        }
    }

    const std::unordered_map<std::string, SocketUnit>& all_socket_units() const {
        return socket_units_;
    }

private:
    void expand_specifiers(SocketUnit& sock) {
        SpecifierContext ctx(sock.name, sock.path);
        for (auto& le : sock.listen)
            le.address = spec_resolver_.resolve(le.address, ctx);
        if (!sock.service.empty())
            sock.service = spec_resolver_.resolve(sock.service, ctx);
    }

    std::string resolve_pair(const SocketUnit& sock) {
        auto& s = this->self();
        if (!sock.service.empty()) return sock.service;
        auto dot = sock.name.rfind('.');
        std::string stem = (dot != std::string::npos) ? sock.name.substr(0,dot) : sock.name;
        std::string direct = stem + ".service";
        if (s.find_unit(direct)) return direct;
        std::string tmpl = stem + "@.service";
        if (s.find_unit(tmpl)) return tmpl;
        return direct;
    }

    bool pairs_with(const SocketUnit& sock, const std::string& svc) {
        if (!sock.service.empty()) return sock.service == svc;
        auto dot = sock.name.rfind('.');
        std::string stem = (dot != std::string::npos) ? sock.name.substr(0,dot) : sock.name;
        return (stem+".service"==svc)||(stem+"@.service"==svc);
    }

    SocketUnit parse_socket_unit(const UnitFile& unit) {
        SocketUnit sock;
        sock.name=unit.name; sock.path=unit.path;
        sock.description=unit.description;
        sock.wants=unit.wants; sock.requires_=unit.requires_;
        sock.after=unit.after; sock.before=unit.before;
        sock.wanted_by=unit.wanted_by; sock.required_by=unit.required_by;

        auto it = unit.raw_sections.find("Socket");
        if (it == unit.raw_sections.end()) return sock;
        const auto& sec = it->second;

        auto get = [&](const char* k) -> std::string_view {
            auto jt = sec.find(k);
            return jt != sec.end() ? std::string_view(jt->second) : std::string_view{};
        };

        // Split on \x1f (RS) for repeated keys, then also on spaces
        auto add_listen = [&](std::string_view raw, ListenType lt) {
            if (raw.empty()) return;
            // May have RS separators from repeated directives
            std::string r(raw);
            for (char& c : r) if (c=='\x1f') c=' ';
            std::string_view sv = r;
            size_t pos=0;
            while (pos < sv.size()) {
                auto s2 = sv.find_first_not_of(" \t", pos);
                if (s2 == std::string_view::npos) break;
                auto e  = sv.find_first_of(" \t", s2);
                if (e  == std::string_view::npos) e = sv.size();
                ListenEntry le; le.type=lt; le.address=std::string(sv.substr(s2,e-s2));
                sock.listen.push_back(std::move(le));
                pos = e;
            }
        };

        add_listen(get("ListenStream"),          ListenType::Stream);
        add_listen(get("ListenDatagram"),         ListenType::Datagram);
        add_listen(get("ListenSequentialPacket"), ListenType::SequentialPacket);
        add_listen(get("ListenFIFO"),             ListenType::FIFO);
        add_listen(get("ListenSpecial"),          ListenType::Special);
        add_listen(get("ListenNetlink"),          ListenType::Netlink);
        add_listen(get("ListenMessageQueue"),     ListenType::MessageQueue);

        if(auto v=get("Accept");         !v.empty()) sock.accept         =detail::parse_bool(v);
        if(auto v=get("Backlog");        !v.empty()) sock.backlog        =detail::parse_int(v,128);
        if(auto v=get("MaxConnections"); !v.empty()) sock.max_conn       =detail::parse_int(v);
        if(auto v=get("MaxConnectionsPerSource");!v.empty()) sock.max_conn_per_source=detail::parse_int(v);
        if(auto v=get("SocketMode");     !v.empty()) sock.socket_mode    =detail::parse_octal(v,0666);
        if(auto v=get("DirectoryMode"); !v.empty()) sock.directory_mode  =detail::parse_octal(v,0755);
        if(auto v=get("ReusePort");      !v.empty()) sock.reuse_port     =detail::parse_bool(v);
        if(auto v=get("FreeBind");       !v.empty()) sock.free_bind      =detail::parse_bool(v);
        if(auto v=get("Transparent");    !v.empty()) sock.transparent    =detail::parse_bool(v);
        if(auto v=get("Broadcast");      !v.empty()) sock.broadcast      =detail::parse_bool(v);
        if(auto v=get("PassCredentials");!v.empty()) sock.pass_credentials=detail::parse_bool(v);
        if(auto v=get("PassSecurity");   !v.empty()) sock.pass_security  =detail::parse_bool(v);
        if(auto v=get("KeepAlive");      !v.empty()) sock.keep_alive     =detail::parse_bool(v);
        if(auto v=get("NoDelay");        !v.empty()) sock.no_delay       =detail::parse_bool(v);
        if(auto v=get("Priority");       !v.empty()) sock.priority       =detail::parse_int(v);
        if(auto v=get("ReceiveBuffer");  !v.empty()) sock.receive_buffer =detail::parse_int(v);
        if(auto v=get("SendBuffer");     !v.empty()) sock.send_buffer    =detail::parse_int(v);
        if(auto v=get("IPTOS");          !v.empty()) sock.iptos          =detail::parse_int(v,-1);
        if(auto v=get("IPTTL");          !v.empty()) sock.ipttl          =detail::parse_int(v,-1);
        if(auto v=get("Mark");           !v.empty()) sock.mark           =detail::parse_int(v,-1);
        if(auto v=get("SocketUser");     !v.empty()) sock.socket_user    =std::string(v);
        if(auto v=get("SocketGroup");    !v.empty()) sock.socket_group   =std::string(v);
        if(auto v=get("Service");        !v.empty()) sock.service        =std::string(v);
        if(auto v=get("BindIPv6Only");   !v.empty()) {
            if(v=="ipv6-only") sock.ipv6only=BindIPv6Only::Ipv6Only;
            else if(v=="both") sock.ipv6only=BindIPv6Only::Both;
        }
        return sock;
    }

    void bind_socket_unit(SocketUnit& sock) {
        auto& s = this->self();
        for (auto& le : sock.listen) {
            if (le.address.find('%') != std::string::npos) continue;
            int fd = create_and_bind(sock, le);
            if (fd < 0) {
                bool hw = (le.type==ListenType::Netlink || le.type==ListenType::MessageQueue);
                s.log_fmt(hw ? LogLevel::Warning : LogLevel::Error,
                    "socket", "{}: failed to bind {}: {}", sock.name, le.address, strerror(errno));
                continue;
            }
            detail::set_cloexec(fd, true);
            le.fd = fd;
            s.log_fmt(LogLevel::Info, "socket", "{}: bound fd {} → {}", sock.name, fd, le.address);
        }
    }

    int create_and_bind(const SocketUnit& sock, const ListenEntry& le) {
        switch (le.type) {
        case ListenType::Stream:
        case ListenType::Datagram:
        case ListenType::SequentialPacket:
            return (!le.address.empty() && (le.address[0]=='/'||le.address[0]=='@'))
                ? bind_unix(sock,le) : bind_inet(sock,le);
        case ListenType::FIFO:    return create_fifo(sock,le);
        case ListenType::Special: return open_special(le);
        case ListenType::Netlink: return bind_netlink(sock,le);
        case ListenType::MessageQueue: return open_mqueue(sock,le);
        }
        return -1;
    }

    int bind_unix(const SocketUnit& sock, const ListenEntry& le) {
        int type=(le.type==ListenType::Datagram)?SOCK_DGRAM:
                  (le.type==ListenType::SequentialPacket)?SOCK_SEQPACKET:SOCK_STREAM;
        int fd=::socket(AF_UNIX,type|SOCK_CLOEXEC,0);
        if(fd<0)return -1;
        apply_opts(fd,sock);
        bool abst=(le.address[0]=='@');
        sockaddr_un sun{}; sun.sun_family=AF_UNIX;
        if(abst){
            sun.sun_path[0]='\0';
            strncpy(sun.sun_path+1,le.address.c_str()+1,sizeof(sun.sun_path)-2);
            socklen_t len=offsetof(sockaddr_un,sun_path)+1+strlen(le.address.c_str()+1);
            if(::bind(fd,(sockaddr*)&sun,len)<0){::close(fd);return -1;}
        } else {
            ::unlink(le.address.c_str());
            auto sl=le.address.rfind('/');
            if(sl!=std::string::npos) ::mkdir(std::string(le.address,0,sl).c_str(),sock.directory_mode);
            strncpy(sun.sun_path,le.address.c_str(),sizeof(sun.sun_path)-1);
            if(::bind(fd,(sockaddr*)&sun,sizeof(sun))<0){::close(fd);return -1;}
            ::chmod(le.address.c_str(),sock.socket_mode);
        }
        if(type!=SOCK_DGRAM && ::listen(fd,sock.backlog)<0){::close(fd);return -1;}
        return fd;
    }

    int bind_inet(const SocketUnit& sock, const ListenEntry& le) {
        auto [host,port]=detail::parse_inet_addr(le.address);
        if(!port)return -1;
        int type=(le.type==ListenType::Datagram)?SOCK_DGRAM:
                  (le.type==ListenType::SequentialPacket)?SOCK_SEQPACKET:SOCK_STREAM;
        bool v6=host.empty()||host.find(':')!=std::string::npos;
        int fd=v6?::socket(AF_INET6,type|SOCK_CLOEXEC,0):-1;
        if(fd>=0&&sock.ipv6only!=BindIPv6Only::Default){
            int o=(sock.ipv6only==BindIPv6Only::Ipv6Only)?1:0;
            ::setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&o,sizeof(o));
        }
        if(fd<0){fd=::socket(AF_INET,type|SOCK_CLOEXEC,0);v6=false;}
        if(fd<0)return -1;
        apply_opts(fd,sock);
        if(v6){
            sockaddr_in6 sa{}; sa.sin6_family=AF_INET6; sa.sin6_port=htons((uint16_t)port);
            if(host.empty()) sa.sin6_addr=IN6ADDR_ANY_INIT;
            else ::inet_pton(AF_INET6,host.c_str(),&sa.sin6_addr);
            if(::bind(fd,(sockaddr*)&sa,sizeof(sa))<0){::close(fd);return -1;}
        } else {
            sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)port);
            sa.sin_addr.s_addr = host.empty() ? INADDR_ANY : ::inet_addr(host.c_str());
            if(::bind(fd,(sockaddr*)&sa,sizeof(sa))<0){::close(fd);return -1;}
        }
        if(type!=SOCK_DGRAM && ::listen(fd,sock.backlog)<0){::close(fd);return -1;}
        return fd;
    }

    int bind_netlink(const SocketUnit&, const ListenEntry& le) {
        std::string_view a=le.address;
        int fam=NETLINK_KOBJECT_UEVENT; uint32_t grp=0;
        auto c=a.find(':');
        if(c!=std::string_view::npos){
            fam=detail::parse_netlink_family(a.substr(0,c));
            std::from_chars(a.data()+c+1,a.data()+a.size(),grp);
        } else fam=detail::parse_netlink_family(a);
        int fd=::socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,fam);
        if(fd<0)return -1;
        sockaddr_nl sa{}; sa.nl_family=AF_NETLINK; sa.nl_groups=grp;
        if(::bind(fd,(sockaddr*)&sa,sizeof(sa))<0){::close(fd);return -1;}
        return fd;
    }

    int create_fifo(const SocketUnit& sock, const ListenEntry& le) {
        ::unlink(le.address.c_str());
        if(::mkfifo(le.address.c_str(),sock.socket_mode)<0)return -1;
        return ::open(le.address.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
    }
    int open_special(const ListenEntry& le) {
        return ::open(le.address.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
    }
    int open_mqueue(const SocketUnit& sock, const ListenEntry& le) {
        struct mq_attr a{}; a.mq_maxmsg=10; a.mq_msgsize=8192;
        return (int)::mq_open(le.address.c_str(),O_RDWR|O_CREAT|O_NONBLOCK,sock.socket_mode,&a);
    }

    void apply_opts(int fd, const SocketUnit& s) {
        int on=1;
        ::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
        if(s.reuse_port)       ::setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,  &on,sizeof(on));
        if(s.free_bind)        ::setsockopt(fd,IPPROTO_IP,IP_FREEBIND,   &on,sizeof(on));
        if(s.transparent)      ::setsockopt(fd,IPPROTO_IP,IP_TRANSPARENT,&on,sizeof(on));
        if(s.broadcast)        ::setsockopt(fd,SOL_SOCKET,SO_BROADCAST,  &on,sizeof(on));
        if(s.pass_credentials) ::setsockopt(fd,SOL_SOCKET,SO_PASSCRED,   &on,sizeof(on));
        if(s.keep_alive)       ::setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,  &on,sizeof(on));
        if(s.no_delay)         ::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,  &on,sizeof(on));
        if(s.priority>=0)      ::setsockopt(fd,SOL_SOCKET,SO_PRIORITY,   &s.priority,      sizeof(s.priority));
        if(s.receive_buffer>0) ::setsockopt(fd,SOL_SOCKET,SO_RCVBUF,     &s.receive_buffer,sizeof(s.receive_buffer));
        if(s.send_buffer>0)    ::setsockopt(fd,SOL_SOCKET,SO_SNDBUF,     &s.send_buffer,   sizeof(s.send_buffer));
        if(s.mark>=0)          ::setsockopt(fd,SOL_SOCKET,SO_MARK,       &s.mark,          sizeof(s.mark));
        if(s.iptos>=0)         ::setsockopt(fd,IPPROTO_IP,IP_TOS,        &s.iptos,         sizeof(s.iptos));
        if(s.ipttl>=0)         ::setsockopt(fd,IPPROTO_IP,IP_TTL,        &s.ipttl,         sizeof(s.ipttl));
#ifdef SO_PASSSEC
        if(s.pass_security)    ::setsockopt(fd,SOL_SOCKET,SO_PASSSEC,    &on,sizeof(on));
#endif
    }

    static std::vector<std::string> split_exec(const std::string& s) {
        std::vector<std::string> r; std::string t; bool iq=false; char qc=0;
        for(char c:s){
            if(iq){if(c==qc)iq=false;else t+=c;}
            else if(c=='"'||c=='\''){iq=true;qc=c;}
            else if(c==' '||c=='\t'){if(!t.empty()){r.push_back(t);t.clear();}}
            else t+=c;
        }
        if(!t.empty())r.push_back(t); return r;
    }
};

} // namespace tinit