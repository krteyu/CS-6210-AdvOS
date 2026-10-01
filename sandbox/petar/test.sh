#!/bin/bash
# ./test.sh [clients] [seconds each client holds its mailbox]
N=${1:-10}
HOLD=${2:-2}

run_clients() {
    PIDS=""
    for i in $(seq $1); do
        ./client $HOLD &
        PIDS="$PIDS $!"
    done
    wait $PIDS
}

./server &
SERVER=$!
sleep 0.5

echo "--- $N clients at once"
run_clients $N
sleep 0.5

# echo "--- 3 more clients, should reuse freed mailboxes"
# run_clients 3
# sleep 0.5

kill -INT $SERVER
wait $SERVER
# ipcs -m -s
