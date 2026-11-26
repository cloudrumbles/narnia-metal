//! UDP kernel-socket tuning applied before quinn takes ownership of the
//! socket. All three knobs are unconditional defaults — no feature flag.
//!
//! - SO_RCVBUF / SO_SNDBUF: 4 MB each. The kernel may halve these on Linux
//!   (doubled internally); we ask for 4 MB so the effective value is ≥ 2 MB
//!   even on tight-policy systems.
//! - UDP_SEGMENT (GSO): tells the kernel we want to batch-send segments of
//!   `segment_size` bytes, amortising syscall overhead on the send path.
//!   Linux ≥ 4.18. On older kernels / non-Linux the setsockopt fails silently.
//! - UDP_GRO: tells the kernel to coalesce incoming UDP datagrams into larger
//!   receive buffers, reducing per-datagram overhead on the recv path.
//!   Linux ≥ 5.10. Same silent-fail policy.

use std::os::fd::AsFd;

/// 4 MB socket buffer target — applied to both SO_RCVBUF and SO_SNDBUF.
pub const UDP_BUF_BYTES: usize = 4 * 1024 * 1024;

/// Segment size hint passed to UDP_SEGMENT (GSO). 1350 bytes fits inside a
/// standard 1500-byte Ethernet MTU with room for IPv6 + UDP headers, and
/// matches the default QUIC max-datagram-size quinn uses.
pub const GSO_SEGMENT_SIZE: u16 = 1350;

/// Bump SO_RCVBUF and SO_SNDBUF to [`UDP_BUF_BYTES`] on any file descriptor
/// that implements `AsFd` — works with `std::net::UdpSocket`,
/// `socket2::Socket`, and anything else the platform exposes.
pub fn bump_udp_buffers<S: AsFd>(socket: &S) -> std::io::Result<()> {
    let sock_ref = socket2::SockRef::from(socket);
    sock_ref.set_recv_buffer_size(UDP_BUF_BYTES)?;
    sock_ref.set_send_buffer_size(UDP_BUF_BYTES)?;
    Ok(())
}

/// Enable UDP GSO (Generic Segmentation Offload) via `UDP_SEGMENT`. Fails
/// silently on kernels that don't support it so the caller never has to worry
/// about conditional compilation.
#[cfg(target_os = "linux")]
pub fn enable_udp_gso<S: AsFd>(socket: &S, segment_size: u16) {
    use std::os::fd::AsRawFd;
    // UDP_SEGMENT = 103 (linux/udp.h). Not exposed by libc yet on all
    // versions, so we define it ourselves.
    const UDP_SEGMENT: libc::c_int = 103;
    let fd: libc::c_int = socket.as_fd().as_raw_fd();
    let val: u16 = segment_size;
    let ret = unsafe {
        libc::setsockopt(
            fd,
            libc::IPPROTO_UDP,
            UDP_SEGMENT,
            &val as *const u16 as *const libc::c_void,
            std::mem::size_of::<u16>() as libc::socklen_t,
        )
    };
    if ret != 0 {
        tracing::warn!(
            errno = %std::io::Error::last_os_error(),
            "UDP_SEGMENT (GSO) setsockopt failed — continuing without GSO",
        );
    }
}

#[cfg(not(target_os = "linux"))]
pub fn enable_udp_gso<S: AsFd>(_socket: &S, _segment_size: u16) {}

/// Enable UDP GRO (Generic Receive Offload) via `UDP_GRO`. Fails silently on
/// kernels that don't support it.
#[cfg(target_os = "linux")]
pub fn enable_udp_gro<S: AsFd>(socket: &S) {
    use std::os::fd::AsRawFd;
    const UDP_GRO: libc::c_int = 104;
    let fd: libc::c_int = socket.as_fd().as_raw_fd();
    let val: libc::c_int = 1;
    let ret = unsafe {
        libc::setsockopt(
            fd,
            libc::IPPROTO_UDP,
            UDP_GRO,
            &val as *const libc::c_int as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        )
    };
    if ret != 0 {
        tracing::warn!(
            errno = %std::io::Error::last_os_error(),
            "UDP_GRO setsockopt failed — continuing without GRO",
        );
    }
}

#[cfg(not(target_os = "linux"))]
pub fn enable_udp_gro<S: AsFd>(_socket: &S) {}

#[cfg(target_vendor = "apple")]
pub fn mark_interactive_video<S: AsFd>(socket: &S) {
    use std::os::fd::AsRawFd;
    const SO_NET_SERVICE_TYPE: libc::c_int = 0x1116;
    const NET_SERVICE_TYPE_VI: libc::c_int = 3;
    let fd: libc::c_int = socket.as_fd().as_raw_fd();
    let ret = unsafe {
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            SO_NET_SERVICE_TYPE,
            &NET_SERVICE_TYPE_VI as *const libc::c_int as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        )
    };
    if ret != 0 {
        tracing::warn!(
            errno = %std::io::Error::last_os_error(),
            "SO_NET_SERVICE_TYPE setsockopt failed — continuing without interactive-video marking",
        );
    }
}

#[cfg(not(target_vendor = "apple"))]
pub fn mark_interactive_video<S: AsFd>(_socket: &S) {}

/// Bind a UDP socket on `addr`, apply all three tunings, and return a
/// `std::net::UdpSocket` ready to hand to quinn.
///
/// Tuning order: bind → SO_RCVBUF/SO_SNDBUF → UDP_SEGMENT → UDP_GRO.
/// All tunings after bind are best-effort; failures are logged but never
/// propagate as errors — quinn still gets the socket regardless.
pub fn bind_tuned_socket(addr: std::net::SocketAddr) -> std::io::Result<std::net::UdpSocket> {
    use socket2::{Domain, Protocol, Socket, Type};

    let domain = if addr.is_ipv6() {
        Domain::IPV6
    } else {
        Domain::IPV4
    };
    let socket = Socket::new(domain, Type::DGRAM, Some(Protocol::UDP))?;

    // Allow dual-stack on IPv6 wildcard addresses so the server can accept
    // both IPv4-mapped and native IPv6 clients on a single socket.
    if addr.is_ipv6() {
        if let Err(e) = socket.set_only_v6(false) {
            tracing::debug!(error = %e, "unable to make socket dual-stack");
        }
    }

    socket.bind(&addr.into())?;

    if let Err(e) = bump_udp_buffers(&socket) {
        tracing::warn!(error = %e, "SO_RCVBUF/SO_SNDBUF bump failed — continuing with OS defaults");
    }
    enable_udp_gso(&socket, GSO_SEGMENT_SIZE);
    enable_udp_gro(&socket);
    mark_interactive_video(&socket);

    // socket2::Socket → std::net::UdpSocket via the From impl (available
    // since socket2 0.4 on Unix and 0.5 on Windows).
    Ok(socket.into())
}
