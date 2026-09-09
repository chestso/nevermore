# tests/tls — committed self-signed test certificate

Used by `test_tls` (loopback negative handshake). Regenerate with:

```sh
openssl req -x509 -newkey rsa:2048 \
    -keyout test-key.pem -out test-cert.pem \
    -days 2 -nodes -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

The client rejects this cert on purpose (system trust store,
hostname verification on) — that refusal is the test. The positive
handshake gate is the manual live probe (`NM_LIVE_TLS=1`).
