#!/usr/bin/env bash
# Generates self-signed EC P-256 client certs for the soundbox device and the backend publisher.
# Event Grid authenticates them by SHA-1 thumbprint (ThumbprintMatch), so no CA is needed.
set -euo pipefail
cd "$(dirname "$0")/../certs"

for name in "$@"; do
  [[ -f "$name.key" ]] && { echo "$name already exists, skipping"; continue; }
  openssl ecparam -name prime256v1 -genkey -noout -out "$name.key"
  openssl req -new -x509 -key "$name.key" -out "$name.pem" -days 365 -subj "/CN=$name"
  openssl x509 -in "$name.pem" -noout -fingerprint -sha1 | sed 's/.*=//; s/://g' > "$name.thumbprint"
  echo "$name thumbprint: $(cat "$name.thumbprint")"
done
