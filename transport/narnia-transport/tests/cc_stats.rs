//! Integration test: spin up a Server + Client on loopback, complete the
//! WebTransport handshake, then verify cc_stats() returns plausible values
//! on both the server-side Publisher and the client-side Subscription.

use std::net::{Ipv4Addr, SocketAddr};
use narnia_transport::{CcStats, Client, Server};

/// Minimal sanity checks on a freshly-read CcStats snapshot.
fn assert_post_handshake(stats: &CcStats, side: &str) {
    // CC algorithm label should contain "Low" (the LowLatency variant name
    // we mirror from the CongestionControl config).
    assert!(
        stats.cc_algorithm.contains("Low"),
        "{side}: expected cc_algorithm to contain 'Low', got {:?}",
        stats.cc_algorithm,
    );

    // After a QUIC handshake there has been at least one ACK round-trip so
    // quinn's smoothed RTT should be > 0. On loopback this is typically
    // in the range 100µs – 5ms.
    assert!(
        stats.smoothed_rtt_us > 0,
        "{side}: smoothed_rtt_us should be > 0 after handshake, got 0",
    );

    // The congestion window starts at a few MTUs (~1200 bytes each). Even
    // the most conservative CC initialiser opens to at least one MTU.
    assert!(
        stats.cwnd_bytes > 1200,
        "{side}: cwnd_bytes should be > 1 MTU (1200 B) after handshake, got {}",
        stats.cwnd_bytes,
    );
}

#[tokio::test]
async fn cc_stats_post_handshake() {
    // Bind on a random loopback port so parallel test runs don't conflict.
    let bind_addr = SocketAddr::new(Ipv4Addr::LOCALHOST.into(), 0);
    let mut server = Server::bind(bind_addr, vec!["localhost".into(), "127.0.0.1".into()])
        .expect("Server::bind");

    let port = server
        .local_addr()
        .expect("local_addr")
        .port();

    // Accept in a background task; connect from the foreground task.
    let server_task = tokio::spawn(async move {
        let session = server
            .accept(1920, 1080, 60.0)
            .await
            .expect("server accept");
        let publisher = session.video_publisher();
        let stats = publisher.cc_stats();
        stats
    });

    let url = format!("https://127.0.0.1:{port}");
    let client = Client::insecure().expect("Client::insecure");
    let subscription = client.connect(&url).await.expect("Client::connect");

    let client_stats = subscription.cc_stats();

    // Also collect server-side stats.
    let server_stats = server_task.await.expect("server task panicked");

    assert_post_handshake(&client_stats, "client");
    assert_post_handshake(&server_stats, "server");

    // delivery_rate may be 0 right at handshake time (no data sent yet)
    // so we don't assert > 0 here. Just check it's a valid u64.
    let _ = client_stats.delivery_rate_bytes_per_sec;

    // bytes_in_flight is stubbed to 0 (quinn-proto 0.11 PathStats has no
    // such field); assert the stub value is exactly 0 so a future upgrade
    // that populates it will make this test fail and remind us to
    // remove/update the stub.
    assert_eq!(
        client_stats.bytes_in_flight, 0,
        "bytes_in_flight stub should be 0 until quinn-proto exposes it",
    );
}
