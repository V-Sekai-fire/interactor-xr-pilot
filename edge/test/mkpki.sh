#!/bin/bash
# Throwaway PKI for the edge gate: a test root, a foreign root, a server leaf, an allowed
# client, an unlisted client, and an allowed-CN client under the foreign root.
set -e
cd "$(dirname "$0")"
O=${OPENSSL:-openssl}
rm -f ./*.pem ./*.csr ./*.srl
ca() { $O req -x509 -newkey rsa:2048 -nodes -keyout $1-key.pem -out $1.pem -days 2 -subj "/CN=$1" \
  -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign 2>/dev/null; }
leaf() {
  printf "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth,clientAuth\nsubjectAltName=%s\n" "$4" > ext.txt
  $O req -new -newkey rsa:2048 -nodes -keyout $1-key.pem -out $1.csr -subj "/CN=$2" 2>/dev/null
  $O x509 -req -in $1.csr -CA $3.pem -CAkey $3-key.pem -CAcreateserial -out $1.pem -days 1 -extfile ext.txt 2>/dev/null
}
ca root
ca foreign
leaf server windows-156928.chibifire.com root "DNS:windows-156928.chibifire.com,IP:127.0.0.1"
leaf mac mac-mini-fire-01b8f5.agents.weftspun root "DNS:mac-mini-fire-01b8f5.agents.weftspun"
leaf other mac-mini-fire-041138.agents.weftspun root "DNS:mac-mini-fire-041138.agents.weftspun"
leaf alien mac-mini-fire-01b8f5.agents.weftspun foreign "DNS:x"
rm -f ext.txt ./*.csr ./*.srl
