# mockex TLS test fixtures

Self-signed RSA-2048 cert + key used by the mockex bench TLS server
(see `benchmarks/mockex/include/mockex/tls_server.hpp`).

**TEST USE ONLY.** Bench client runs with `verify_peer = false`. Do not
ship this anywhere production-adjacent.

## Files

| File         | Purpose                                              |
|--------------|------------------------------------------------------|
| `server.crt` | PEM x509, self-signed, CN=`mockex.bench`             |
| `server.key` | PEM PKCS#8 private key, unencrypted, mode 600        |
| `gen.sh`     | Idempotent regenerator (uses `openssl req -x509 …`)  |
| `server.cnf` | OpenSSL config used by `gen.sh` (regenerated each run) |

## SAN coverage

```
DNS:    localhost, mockex.bench
IP:     127.0.0.1, 172.31.47.238   ← current bench host NIC_A primary IP
```

If the bench host's NIC_A IP changes, edit `gen.sh`'s `[alt]` block and
re-run.

## Validity

10 years (3650 days). Bench cert; regenerate freely if it ever expires
or the host moves.

## Regenerating

```
./benchmarks/mockex/fixtures/tls/gen.sh
git add benchmarks/mockex/fixtures/tls/{server.crt,server.key}
```

## Why a server cert here and not in eph-net?

`eph-net`'s `TlsSession` is **client-only by design** (`TLS_client_method`
hardcoded). Adding server-mode would broaden the public surface beyond
the HFT-client-only intent. Mockex needs a server because it mocks the
exchange; this is bench infrastructure, not core library.
