#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace tinit {

/// Mirrors systemd's ListenX= directives
enum class ListenType : uint8_t {
    Stream,           // ListenStream=       SOCK_STREAM  (TCP or Unix)
    Datagram,         // ListenDatagram=     SOCK_DGRAM   (UDP or Unix)
    SequentialPacket, // ListenSequentialPacket=  SOCK_SEQPACKET (Unix)
    FIFO,             // ListenFIFO=         named pipe
    Special,          // ListenSpecial=      character/block device
    Netlink,          // ListenNetlink=      AF_NETLINK
    MessageQueue,     // ListenMessageQueue= POSIX mqueue
};

/// One Listen* entry — a socket unit may have several
struct ListenEntry {
    ListenType  type;
    std::string address; // "0.0.0.0:22", "/run/foo.sock", "netlink:KOBJECT_UEVENT", etc.
    int         fd = -1; // populated by SocketMixin::bind_phase
    std::string name;    // fd name for LISTEN_FDNAMES (defaults to unit name stem)
};

/// BindIPv6Only= values
enum class BindIPv6Only : uint8_t { Default, Both, Ipv6Only };

/// Parsed [Socket] section
struct SocketUnit {
    // Identity
    std::string name;   // e.g. "sshd.socket"
    std::string path;

    // Listen entries (order-preserving; multiple allowed)
    std::vector<ListenEntry> listen;

    // Activation model
    bool accept      = false;  // Accept=yes  → inetd-style, spawn per connection
    int  backlog     = 128;    // Backlog=
    int  max_conn    = 0;      // MaxConnections=  (0 = unlimited; only with Accept=yes)
    int  max_conn_per_source = 0; // MaxConnectionsPerSource=

    // Socket options
    int  socket_mode        = 0666;    // SocketMode=  (octal)
    int  directory_mode     = 0755;    // DirectoryMode=
    bool reuse_port         = false;   // ReusePort=
    bool free_bind          = false;   // FreeBind=
    bool transparent        = false;   // Transparent=
    bool broadcast          = false;   // Broadcast=
    bool pass_credentials   = false;   // PassCredentials=
    bool pass_security      = false;   // PassSecurity=
    bool keep_alive         = false;   // KeepAlive=
    bool no_delay           = false;   // NoDelay=
    BindIPv6Only ipv6only   = BindIPv6Only::Default; // BindIPv6Only=
    int  priority           = 0;       // Priority=
    int  receive_buffer     = 0;       // ReceiveBuffer=  (0 = kernel default)
    int  send_buffer        = 0;       // SendBuffer=
    int  iptos             = -1;       // IPTOS=
    int  ipttl             = -1;       // IPTTL=
    int  mark              = -1;       // Mark=

    // Ownership
    std::string socket_user;           // SocketUser=
    std::string socket_group;          // SocketGroup=

    // Service pairing
    std::string service;               // Service= override (default: same stem + .service)

    // [Unit] fields (duplicated here for convenience; also in base UnitFile)
    std::string description;
    std::vector<std::string> wants;
    std::vector<std::string> requires_;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::vector<std::string> wanted_by;
    std::vector<std::string> required_by;

    // Runtime state
    int  active_connections = 0;       // for MaxConnections enforcement
    bool activated          = false;   // has paired service been started

    /// Derive the paired service name (foo.socket → foo.service unless Service= set)
    std::string paired_service() const {
        if (!service.empty()) return service;
        auto dot = name.rfind('.');
        return (dot != std::string::npos) ? name.substr(0, dot) + ".service" : name + ".service";
    }
};

} // namespace tinit