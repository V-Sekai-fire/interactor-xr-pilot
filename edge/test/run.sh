#!/bin/bash
# Local gate for mcp_edge: one positive call, four refusals, one overload control.
cd "$(dirname "$0")"
E=${E:-/tmp/eb/mcp_edge}
pkill -f 'up.py 18765'; pkill -f 'mcp_edge serve'; sleep 0.3
DELAY=2 python3 up.py 18765 &
UP=$!
$E serve --cert server.pem --key server-key.pem --root root.pem \
  --allow mac-mini-fire-01b8f5.agents.weftspun --port 18766 --upstream 18765 2> serve.log &
SV=$!
sleep 1
call() {
  timeout 15 $E call --cert $1.pem --key $1-key.pem --root root.pem \
    --server-name ${2:-windows-156928.chibifire.com} --connect 127.0.0.1:18766 2> call.err
  echo "exit=$?"
}
echo "== positive (allowed: 1 request + 1 notification)"
printf '{"jsonrpc":"2.0","id":1,"method":"ping"}\n{"jsonrpc":"2.0","method":"notifications/initialized"}\n' | call mac
echo "== control 1: no client certificate"
echo '{"jsonrpc":"2.0","id":2,"method":"ping"}' | timeout 15 $E call --cert none --key none --root root.pem \
  --server-name windows-156928.chibifire.com --connect 127.0.0.1:18766 2> call.err
echo "exit=$?"
echo "== control 2: allowed CN, foreign root"
echo '{"jsonrpc":"2.0","id":3,"method":"ping"}' | call alien
echo "== control 3: valid chain, unlisted CN"
echo '{"jsonrpc":"2.0","id":4,"method":"ping"}' | call other
echo "== control 4: wrong server name"
echo '{"jsonrpc":"2.0","id":5,"method":"ping"}' | call mac wrong.chibifire.com
echo "== overload: 6 at once, upstream 2 s, 4 credits"
for i in 1 2 3 4 5 6; do echo "{\"jsonrpc\":\"2.0\",\"id\":$((10+i)),\"method\":\"slow\"}"; done | call mac
echo "== server log"
cat serve.log
kill $SV $UP 2>/dev/null
