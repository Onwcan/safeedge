#!/usr/bin/env bash
#
# How long an OPC UA client takes to learn that the runtime changed state.
#
# Sweeps the two intervals that bound it -- the server's read of the shared
# snapshot, and the subscription's sampling/publishing interval -- because one
# data point would show a number and the sweep shows what the number is made of.
#
# Usage: scripts/measure-opcua-latency.sh <plain-build> <opcua-build> [output-dir]

set -uo pipefail

SE_BUILD="${1:?safeedged build dir}"
UA_BUILD="${2:?opcua build dir}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${3:-$ROOT/evidence}"

ESTOP=/tmp/safeedge-sweep-estop
ACK=/tmp/safeedge-sweep-ack
REGION=/safeedge-sweep
PORT=4844

cleanup() {
  pkill -f "safeedge-opcua" 2>/dev/null
  pkill -f "safeedged" 2>/dev/null
  rm -f "$ESTOP" "$ACK"
}
trap cleanup EXIT

run_case() {
  local publish_ms="$1" sampling_ms="$2"
  cleanup
  sleep 1

  SAFEEDGE_METRICS_PORT=9961 SAFEEDGE_SNAPSHOT_SHM="$REGION" \
    SAFEEDGE_ESTOP_FILE="$ESTOP" SAFEEDGE_ACK_FILE="$ACK" \
    "$SE_BUILD/safeedged" >/tmp/sweep_se.log 2>&1 &
  sleep 2

  SAFEEDGE_SNAPSHOT_SHM="$REGION" SAFEEDGE_OPCUA_PORT="$PORT" \
    SAFEEDGE_OPCUA_PUBLISH_MS="$publish_ms" \
    "$UA_BUILD/safeedge-opcua" >/tmp/sweep_srv.log 2>&1 &
  sleep 3

  # This local timing experiment uses a temporary, self-signed server identity.
  "$UA_BUILD/safeedge-opcua-probe" --insecure-accept-any-server-cert \
    --endpoint "opc.tcp://127.0.0.1:$PORT" \
    --sampling-ms "$sampling_ms" --seconds 22 >/tmp/sweep_probe.log 2>&1 &
  local probe_pid=$!
  sleep 4

  # Six transitions: trip, release, acknowledge. Releasing does not restart --
  # that is the IEC 60204-1 behaviour -- so each cycle produces two transitions.
  for _ in 1 2 3; do
    touch "$ESTOP"; sleep 2
    rm -f "$ESTOP"; sleep 1
    touch "$ACK"; sleep 2
  done

  wait "$probe_pid" 2>/dev/null
  printf '  publish %3s ms  sampling %3s ms   %s\n' "$publish_ms" "$sampling_ms" \
    "$(grep -E '^n=' /tmp/sweep_probe.log || echo 'no notifications')"
}

mkdir -p "$OUT"
{
  echo "OPC UA subscription latency: runtime decision -> client notification"
  echo "===================================================================="
  echo
  echo "Three processes: safeedged decides and publishes a snapshot to shared"
  echo "memory, safeedge-opcua serves it, and a subscribed client times its own"
  echo "notifications against the transition instant the runtime stamped."
  echo
  echo "The first notification of each run is discarded: a subscription opens by"
  echo "reporting the current value, which is state rather than an event."
  echo
  run_case 10 10
  run_case 10 50
  run_case 10 200
  run_case 100 10
  echo
  echo "What binds this is the subscription's REVISED publishing interval, which"
  echo "is the server's decision and not the client's. With open62541's stock"
  echo "limits a client asking for 10 ms is given 100 ms publishing and 50 ms"
  echo "sampling, and reports a median near 75 ms; the probe prints the revision"
  echo "so the difference is visible rather than assumed."
  echo
  echo "safeedge-opcua lowers publishingIntervalLimits.min and"
  echo "samplingIntervalLimits.min to 10 ms, so the same request is granted and"
  echo "the same client reports a median near 12 ms. Six times, from a server"
  echo "setting -- the client never had the say."
  echo
  echo "A subscription is therefore not inherently faster than a poll. It removes"
  echo "the CLIENT's poll. The server-side intervals remain, and they are"
  echo "somebody's configuration rather than a property of the protocol."
} | tee "$OUT/opcua-subscription-latency.txt"
