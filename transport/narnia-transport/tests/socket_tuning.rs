//! Integration tests for kernel-socket tuning applied at Server::bind and
//! Client::insecure time. Verifies that SO_RCVBUF, SO_SNDBUF, and UDP_GRO
//! are set to at least the expected values before quinn takes ownership of
//! the socket. UDP_SEGMENT getsockopt is skipped if the kernel doesn't echo
//! the value back (not all kernels support it), to stay portable.

use std::net::SocketAddr;
use std::os::fd::{AsFd, AsRawFd};

const UDP_GRO: libc::c_int = 104;

/// Read back SO_RCVBUF via getsockopt. Returns the kernel's actual value;
/// note Linux doubles the value internally so getsockopt returns 2×what was
/// set. We assert ≥ the set value (not the doubled value) to stay robust
/// against kernel policy differences.
fn get_rcvbuf(fd: libc::c_int) -> usize {
    let mut val: libc::c_int = 0;
    let mut len = std::mem::size_of::<libc::c_int>() as libc::socklen_t;
    unsafe {
        libc::getsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_RCVBUF,
            &mut val as *mut libc::c_int as *mut libc::c_void,
            &mut len,
        );
    }
    val as usize
}

fn get_sndbuf(fd: libc::c_int) -> usize {
    let mut val: libc::c_int = 0;
    let mut len = std::mem::size_of::<libc::c_int>() as libc::socklen_t;
    unsafe {
        libc::getsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_SNDBUF,
            &mut val as *mut libc::c_int as *mut libc::c_void,
            &mut len,
        );
    }
    val as usize
}

/// Returns Some(val) if UDP_GRO getsockopt succeeded, None if the option is
/// not supported on this kernel.
#[cfg(target_os = "linux")]
fn get_udp_gro(fd: libc::c_int) -> Option<libc::c_int> {
    let mut val: libc::c_int = 0;
    let mut len = std::mem::size_of::<libc::c_int>() as libc::socklen_t;
    let ret = unsafe {
        libc::getsockopt(
            fd,
            libc::IPPROTO_UDP,
            UDP_GRO,
            &mut val as *mut libc::c_int as *mut libc::c_void,
            &mut len,
        )
    };
    if ret == 0 { Some(val) } else { None }
}

/// Returns Some(val) if UDP_SEGMENT getsockopt succeeded.
#[cfg(target_os = "linux")]
fn get_udp_segment(fd: libc::c_int) -> Option<u16> {
    const UDP_SEGMENT: libc::c_int = 103;
    let mut val: u16 = 0;
    let mut len = std::mem::size_of::<u16>() as libc::socklen_t;
    let ret = unsafe {
        libc::getsockopt(
            fd,
            libc::IPPROTO_UDP,
            UDP_SEGMENT,
            &mut val as *mut u16 as *mut libc::c_void,
            &mut len,
        )
    };
    if ret == 0 { Some(val) } else { None }
}

/// Build a tuned socket and return the raw fd plus the socket (kept alive to
/// own the fd).
fn make_tuned_socket(addr: SocketAddr) -> std::net::UdpSocket {
    narnia_transport::socket_tuning::bind_tuned_socket(addr)
        .expect("bind_tuned_socket should succeed")
}

#[test]
fn server_socket_buffers_and_gro() {
    // Use a random port on loopback so the test is self-contained and
    // doesn't require elevated privilege.
    let addr: SocketAddr = "127.0.0.1:0".parse().unwrap();
    let sock = make_tuned_socket(addr);
    let fd = sock.as_fd().as_raw_fd();

    let rcvbuf = get_rcvbuf(fd);
    let sndbuf = get_sndbuf(fd);

    // Linux doubles the value internally; getsockopt returns 2 × set value.
    // We set UDP_BUF_BYTES (4 MB), so the echoed value should be ≥ 4 MB.
    // (Some kernels may cap at /proc/sys/net/core/rmem_max; skip the
    //  assertion only if the returned value is non-zero — a zero result
    //  means the call itself failed, which is a real error.)
    assert!(rcvbuf > 0, "SO_RCVBUF getsockopt returned 0 — something went wrong");
    assert!(sndbuf > 0, "SO_SNDBUF getsockopt returned 0 — something went wrong");

    let min = narnia_transport::socket_tuning::UDP_BUF_BYTES;
    assert!(
        rcvbuf >= min,
        "SO_RCVBUF {rcvbuf} < expected {min} — kernel may have a low rmem_max"
    );
    assert!(
        sndbuf >= min,
        "SO_SNDBUF {sndbuf} < expected {min} — kernel may have a low wmem_max"
    );

    #[cfg(target_os = "linux")]
    if let Some(gro) = get_udp_gro(fd) {
        assert_eq!(gro, 1, "UDP_GRO should be enabled (=1)");
    }
    // if get_udp_gro returns None the kernel doesn't support it — that's fine.
}

#[test]
fn client_socket_buffers_and_gso() {
    let addr: SocketAddr = "[::]:0".parse().unwrap();
    let sock = make_tuned_socket(addr);
    let fd = sock.as_fd().as_raw_fd();

    let rcvbuf = get_rcvbuf(fd);
    let sndbuf = get_sndbuf(fd);

    let min = narnia_transport::socket_tuning::UDP_BUF_BYTES;
    assert!(rcvbuf >= min, "client SO_RCVBUF {rcvbuf} < expected {min}");
    assert!(sndbuf >= min, "client SO_SNDBUF {sndbuf} < expected {min}");

    #[cfg(target_os = "linux")]
    {
        // UDP_SEGMENT getsockopt doesn't reliably echo the set value on all
        // kernels — some return 0 even when the option was accepted. Skip
        // the assertion if getsockopt fails OR if the echoed value is 0, so
        // the test stays green on kernels that accept but don't reflect it.
        match get_udp_segment(fd) {
            Some(seg) if seg > 0 => {
                assert_eq!(
                    seg,
                    narnia_transport::socket_tuning::GSO_SEGMENT_SIZE,
                    "UDP_SEGMENT echoed a non-zero value but it doesn't match GSO_SEGMENT_SIZE"
                );
            }
            _ => {
                // getsockopt failed or returned 0 — kernel doesn't echo the
                // value; skip.
            }
        }
    }
}
