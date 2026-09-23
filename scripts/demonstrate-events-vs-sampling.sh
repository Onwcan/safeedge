#!/usr/bin/env bash
#
# A subscription on a variable does not deliver changes. It delivers samples.
#
# The server reads the node every samplingInterval and reports the value if it
# differs from the one it read last time. A state that begins and ends between
# two samples is not delayed and not merged -- it is absent, and the client has
# no way to know it existed. For a diagnostic gauge that is a fair trade. For
# "the machine tripped and recovered", it is the difference between a log and a
# fiction.
#
# This runs both subscriptions in ONE client session against ONE server, so the
# two are looking at exactly the same transitions and a difference in what they
# report is a difference between the mechanisms.
#
# Usage: scripts/demonstrate-events-vs-sampling.sh <plain-build> <opcua-build> [out-dir]

set -uo pipefail

SE_BUILD="${1:?safeedged build dir}"
UA_BUILD="${2:?opcua build dir}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${3:-$ROOT/evidence}"

ESTOP=/tmp/safeedge-ev-estop
ACK=/tmp/safeedge-ev-ack
REGION=/safeedge-events
PORT=4848
# Long enough that a sampler at this interval cannot see a state that lasts
# 150 ms, which is the whole experiment.
SAMPLING_MS=500
# Short enough that the SERVER sees every transition, so anything the client
# misses was lost by the client and not before it reached OPC UA.
PUBLISH_MS=10
CYCLES=6
FAILURES=0

cleanup() {
  pkill -f "safeedge-opcua" 2>/dev/null
  pkill -f "safeedged" 2>/dev/null
  rm -f "$ESTOP" "$ACK"
}
trap cleanup EXIT

check() {
  local label="$1" verdict="$2" detail="$3"
  if [ "$verdict" = "ok" ]; then
    printf '  PASS  %-50s %s\n' "$label" "$detail"
  else
    printf '  FAIL  %-50s %s\n' "$label" "$detail"
    FAILURES=$((FAILURES + 1))
  fi
}

mkdir -p "$OUT"
cleanup
sleep 1

SAFEEDGE_METRICS_PORT=9965 SAFEEDGE_SNAPSHOT_SHM="$REGION" \
  SAFEEDGE_ESTOP_FILE="$ESTOP" SAFEEDGE_ACK_FILE="$ACK" \
  "$SE_BUILD/safeedged" >/tmp/ev_se.log 2>&1 &
sleep 2

SAFEEDGE_SNAPSHOT_SHM="$REGION" SAFEEDGE_OPCUA_PORT="$PORT" \
  SAFEEDGE_OPCUA_PUBLISH_MS="$PUBLISH_MS" \
  "$UA_BUILD/safeedge-opcua" >/tmp/ev_srv.log 2>&1 &
sleep 3

# This local delivery experiment uses a temporary, self-signed server identity.
"$UA_BUILD/safeedge-opcua-probe" --insecure-accept-any-server-cert \
  --endpoint "opc.tcp://127.0.0.1:$PORT" \
  --sampling-ms "$SAMPLING_MS" --seconds 20 --events >/tmp/ev_probe.log 2>&1 &
PROBE_PID=$!
sleep 4

# Three actions, TWO transitions. Releasing the e-stop does not change the
# safety state -- IEC 60204-1 requires that restoring the switch must not
# restart the machine, so the runtime stays tripped until it is acknowledged.
# The release being a non-event is the latch working, and a first version of
# this script expected three transitions per cycle and was wrong by exactly
# that.
#
# States last 100 ms: comfortably visible to a server polling at 10 ms, and
# well inside a single 500 ms client sample. The cycles run back to back so
# several transitions fall into the same sampling window, which is the case a
# sampler cannot represent no matter how the value is encoded.
for _ in $(seq "$CYCLES"); do
  touch "$ESTOP"; sleep 0.1
  rm -f "$ESTOP"; sleep 0.05
  touch "$ACK";   sleep 0.1
done

wait "$PROBE_PID" 2>/dev/null

EXPECTED=$((CYCLES * 2))
COUNTS_LINE="$(grep -E '^counts:' /tmp/ev_probe.log || true)"
DATA_N="$(printf '%s' "$COUNTS_LINE" | sed -n 's/^counts: \([0-9]*\) data-change.*/\1/p')"
EVENT_N="$(printf '%s' "$COUNTS_LINE" | sed -n 's/.*notification(s), \([0-9]*\) event.*/\1/p')"
COALESCED="$(grep -oE '^ *[0-9]+ transition\(s\) the SERVER' /tmp/ev_probe.log \
  | grep -oE '[0-9]+' | head -1)"
DATA_N="${DATA_N:-0}"; EVENT_N="${EVENT_N:-0}"; COALESCED="${COALESCED:-0}"

{
  echo "OPC UA events versus a sampled variable"
  echo "======================================="
  echo
  echo "One server, one client session, two subscriptions. The runtime was made"
  echo "to change safety state $EXPECTED times ($CYCLES trip/acknowledge cycles, two"
  echo "transitions each -- releasing the e-stop is not one, which is the latch"
  echo "doing its job), in states lasting 100 ms, run back to back."
  echo
  echo "  server snapshot poll   ${PUBLISH_MS} ms   (so the server sees every transition)"
  echo "  client sampling        ${SAMPLING_MS} ms   (so the client cannot)"
  echo
  printf '  transitions produced          %s\n' "$EXPECTED"
  printf '  events received               %s\n' "$EVENT_N"
  printf '  data-change notifications     %s\n' "$DATA_N"
  printf '  coalesced by the server       %s\n' "$COALESCED"
  echo

  if [ "$EVENT_N" -eq "$EXPECTED" ]; then
    check "every transition arrived as an event" ok "$EVENT_N of $EXPECTED"
  else
    check "every transition arrived as an event" bad "$EVENT_N of $EXPECTED"
  fi

  if [ "$DATA_N" -lt "$EVENT_N" ]; then
    check "the sampled variable reported fewer" ok "$DATA_N vs $EVENT_N"
  else
    check "the sampled variable reported fewer" bad "$DATA_N vs $EVENT_N"
  fi

  # If the server coalesced anything, the comparison above is contaminated: both
  # subscriptions would have lost the same transitions for the same reason, and
  # the difference would not be about sampling at all.
  if [ "$COALESCED" -eq 0 ]; then
    check "the server itself missed nothing" ok "0 coalesced"
  else
    check "the server itself missed nothing" bad "$COALESCED coalesced"
  fi
  echo
  echo "What this is and is not"
  echo "-----------------------"
  echo "It is not a latency result. Both subscriptions here share one publishing"
  echo "interval, so the events arrive no sooner than the samples do. What"
  echo "differs is how many arrive at all: a sampled variable reports the value"
  echo "it happens to read, so a state that begins and ends between two reads"
  echo "was never there. The usual framing of events as an optimisation has this"
  echo "backwards -- the difference is correctness, and the latency is the same."
  echo
  echo "An event queue can overflow too. This client asked for 64 and discards"
  echo "the newest rather than the oldest, because for a safety log the first"
  echo "thing that went wrong is worth more than the most recent one. Lossless"
  echo "up to the depth you asked for is a weaker promise than lossless, and a"
  echo "stronger one than a sampler can make at any depth."
  echo
  echo "And the chain is only as event-driven as its most sampled link. This"
  echo "server reads its snapshot on a poll, so a transition shorter than"
  echo "${PUBLISH_MS} ms never reaches OPC UA at all. That case is detectable rather"
  echo "than silent: the snapshot carries a transition counter, and when it"
  echo "advances by more than one the server reports the gap in"
  echo "MissedTransitions instead of presenting the newest state as the whole"
  echo "story."
  echo
  if [ "$FAILURES" -eq 0 ]; then
    echo "All checks held."
  else
    echo "$FAILURES check(s) did not hold."
  fi
} | tee "$OUT/opcua-events-vs-sampling.txt"

exit "$FAILURES"
