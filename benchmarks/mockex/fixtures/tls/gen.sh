#!/usr/bin/env bash
# Generate a self-signed RSA-2048 cert for the mockex bench TLS server.
#
# **TEST USE ONLY**: this cert ships with `verify_peer = false` on the
# client side. It is not, and is not intended to be, a production
# certificate.
#
# The cert covers IP SANs for:
#   - 127.0.0.1                   (loopback smoke tests)
#   - 172.31.47.238               (current bench host NIC_A primary IP)
# DNS SANs:
#   - localhost
#   - mockex.bench                (CN; arbitrary, here for SNI logs)
#
# To regenerate (idempotent):
#   ./benchmarks/mockex/fixtures/tls/gen.sh
#
# Validity is 10 years (3650d) — bench cert, regenerate freely if the
# host IP changes.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CERT="server.crt"
KEY="server.key"
CONF="server.cnf"

cat > "$CONF" <<'EOF'
[req]
distinguished_name = dn
prompt             = no
x509_extensions    = ext

[dn]
CN = mockex.bench

[ext]
subjectAltName       = @alt
basicConstraints     = CA:FALSE
keyUsage             = digitalSignature, keyEncipherment
extendedKeyUsage     = serverAuth

[alt]
DNS.1 = localhost
DNS.2 = mockex.bench
IP.1  = 127.0.0.1
IP.2  = 172.31.47.238
EOF

openssl req \
    -x509 -nodes \
    -newkey rsa:2048 \
    -keyout "$KEY" \
    -out "$CERT" \
    -days 3650 \
    -config "$CONF"

# Lock down the private key — it's a test cert but no need to leave
# it world-readable.
chmod 600 "$KEY"
chmod 644 "$CERT"

echo
echo "Generated:"
ls -l "$CERT" "$KEY"
echo
echo "Cert summary:"
openssl x509 -in "$CERT" -noout -subject -issuer -dates -ext subjectAltName | sed 's/^/  /'
