#!/usr/bin/env bash
set -euo pipefail

HOST=${HOST:-127.0.0.1}
PORT=${PORT:-9090}
CLIENTS=${1:-10}
REQUESTS=${2:-100}

start_ns=$(date +%s%N)

for _ in $(seq 1 "$CLIENTS"); do
  (
    for _ in $(seq 1 "$REQUESTS"); do
      ./bin/client "$HOST" "$PORT" PING >/dev/null
    done
  ) &
done

wait

end_ns=$(date +%s%N)
elapsed_ns=$((end_ns - start_ns))
elapsed_s=$(awk "BEGIN {print $elapsed_ns/1000000000}")

total=$((CLIENTS * REQUESTS))
req_per_sec=$(awk "BEGIN {print $total/$elapsed_s}")

echo "total_requests=$total"
echo "elapsed_seconds=$elapsed_s"
echo "requests_per_sec=$req_per_sec"
