#!/bin/bash
# Gate for mcp_edge: one admitted call, four refusals, one overload control.
# Every outcome is asserted; the script exits 1 on the first mismatch.
cd "$(dirname "$0")"
E=${E:-../../build-edge/mcp_edge}
fails=0
pkill -f 'up.py 18765' 2>/dev/null; pkill -f 'mcp_edge serve' 2>/dev/null; sleep 0.3
DELAY=2 python3 up.py 18765 &
UP=$!
$E serve --cert server.pem --key server-key.pem --root root.pem \
  --allow mac-mini-fire-01b8f5.agents.weftspun --port 18766 --upstream 18765 2> serve.log &
SV=$!
sleep 1
call() { # call <client> [server-name]; stdin is the messages
  local cert=$1.pem key=$1-key.pem
  [ "$1" = none ] && cert=none key=none
  timeout 20 $E call --cert $cert --key $key --root root.pem \
    --server-name ${2:-windows-156928.chibifire.com} --connect 127.0.0.1:18766 2> call.err
}
check() { # check <name> <condition result 0|1>
  if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}
out=$(printf '{"jsonrpc":"2.0","id":1,"method":"ping"}\n{"jsonrpc":"2.0","method":"notifications/initialized"}\n' | call mac); rc=$?
[ $rc = 0 ] && [ "$(echo "$out" | grep -c '"id": 1')" = 1 ] && [ "$(echo "$out" | wc -l | tr -d ' ')" = 1 ]; check "admitted call: one reply for one request, none for the notification" $?
for c in "none|no client certificate" "alien|allowed CN under a foreign root" "other|valid chain, unlisted CN"; do
  echo '{"jsonrpc":"2.0","id":2,"method":"ping"}' | call ${c%%|*} > /dev/null; rc=$?
  [ $rc = 1 ]; check "refused: ${c#*|}" $?
done
echo '{"jsonrpc":"2.0","id":3,"method":"ping"}' | call mac wrong.chibifire.com > /dev/null; rc=$?
[ $rc = 1 ]; check "refused: wrong server name" $?
out=$(for i in 1 2 3 4 5 6; do echo "{\"jsonrpc\":\"2.0\",\"id\":$((10+i)),\"method\":\"slow\"}"; done | call mac); rc=$?
busy=$(echo "$out" | grep -c 'edge busy'); done_=$(echo "$out" | grep -c '"echo": "slow"')
[ $rc = 0 ] && [ "$busy" = 2 ] && [ "$done_" = 4 ] && [ "$(echo "$out" | grep 'edge busy' | grep -c '"id":null')" = 0 ]; check "overload: 4 served, 2 edge busy carrying their ids" $?
grep -q 'refused: no client certificate' serve.log && grep -q 'not on the allow list' serve.log && grep -q 'chain does not verify' serve.log; check "server log names each refusal" $?
kill $SV $UP 2>/dev/null
echo "--- serve.log"; cat serve.log
[ $fails = 0 ] && echo "PASS" || { echo "FAILED: $fails"; exit 1; }
