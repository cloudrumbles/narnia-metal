use std::time::Duration;

use narnia_transport::client::Client;
use narnia_transport::server::{Server, SessionHandle};

fn hash_bytes(hex: &str) -> [u8; 32] {
    let mut out = [0u8; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).expect("server hash is hex");
    }
    out
}

fn spawn_accept(mut server: Server) -> tokio::task::JoinHandle<Option<SessionHandle>> {
    tokio::spawn(async move { server.accept(960, 608, 30.0).await.ok() })
}

#[tokio::test]
async fn pinned_client_connects_when_the_cert_hash_matches() {
    let server = Server::bind("127.0.0.1:0".parse().expect("addr"), vec![]).expect("bind");
    let port = server.local_addr().expect("local addr").port();
    let hash = hash_bytes(&server.cert_sha256_hex());
    let accept = spawn_accept(server);

    let client = Client::pinned(hash).expect("pinned client");
    let subscription = tokio::time::timeout(
        Duration::from_secs(5),
        client.connect(&format!("https://127.0.0.1:{port}")),
    )
    .await
    .expect("connect finished in time")
    .expect("connect succeeded");

    assert_eq!(subscription.hello().width, 960);
    assert_eq!(subscription.hello().height, 608);
    let handle = accept.await.expect("accept task");
    assert!(handle.is_some());
}

#[tokio::test]
async fn pinned_client_refuses_a_server_with_a_different_cert() {
    let server = Server::bind("127.0.0.1:0".parse().expect("addr"), vec![]).expect("bind");
    let port = server.local_addr().expect("local addr").port();
    let mut hash = hash_bytes(&server.cert_sha256_hex());
    hash[0] ^= 0xff;
    let accept = spawn_accept(server);

    let client = Client::pinned(hash).expect("pinned client");
    let outcome = tokio::time::timeout(
        Duration::from_secs(5),
        client.connect(&format!("https://127.0.0.1:{port}")),
    )
    .await
    .expect("connect finished in time");

    assert!(outcome.is_err());
    accept.abort();
}
