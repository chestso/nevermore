# tests/tls — committed self-signed test certificate

Used by `test_tls` (loopback TLS, both directions). Regenerate with:

```sh
openssl req -x509 -newkey rsa:2048 \
    -keyout test-key.pem -out test-cert.pem \
    -days 3650 -nodes -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
chmod 600 test-key.pem test-cert.pem
```

The lifetime is long ON PURPOSE: the certificate test makes the client
reject this cert (system trust store, hostname verification on) and the
write test makes it trust it (via `NEVERMORE_TLS_CA_FILE`), and a
two-day cert rots into "expired" for the second one — which is how it
went unnoticed that the client never even loaded it. The manual live
probe (`NM_LIVE_TLS=1 ./test_tls -v`) remains the real-endpoint gate.
