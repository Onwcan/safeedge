#!/usr/bin/env bash
#
# Does the trust list actually keep anyone out?
#
# The unit tests prove which configurations are refused. They cannot prove that
# a client without a trusted certificate is turned away, because that happens
# inside open62541 during the SecureChannel handshake, between two processes.
# So this runs the real server and points two real clients at it: one whose
# certificate is in the trust list and one whose certificate is not.
#
# It also checks the refusal that has no runtime symptom at all -- a trust list
# offered alongside SecurityPolicy#None, which every excluded client would
# simply route around.
#
# Usage: scripts/demonstrate-client-auth.sh <plain-build> <opcua-build> [output-dir]

set -uo pipefail

SE_BUILD="${1:?safeedged build dir}"
UA_BUILD="${2:?opcua build dir}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${3:-$ROOT/evidence}"

WORK="$(mktemp -d /tmp/safeedge-auth-XXXXXX)"
REGION=/safeedge-auth
PORT=4846
FAILURES=0

cleanup() {
  pkill -f "safeedge-opcua" 2>/dev/null
  pkill -f "safeedged" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

# Reports a check and remembers whether it held, so one failure does not stop
# the rest -- a partial answer here is worth more than an early exit.
check() {
  local label="$1" expected="$2" actual="$3"
  if [ "$expected" = "$actual" ]; then
    printf '  PASS  %-52s %s\n' "$label" "$actual"
  else
    printf '  FAIL  %-52s expected %s, got %s\n' "$label" "$expected" "$actual"
    FAILURES=$((FAILURES + 1))
  fi
}

start_runtime() {
  SAFEEDGE_METRICS_PORT=9963 SAFEEDGE_SNAPSHOT_SHM="$REGION" \
    "$SE_BUILD/safeedged" >"$WORK/safeedged.log" 2>&1 &
  sleep 2
}

start_server() {
  local trustlist="$1" insecure="${2:-0}"
  SAFEEDGE_SNAPSHOT_SHM="$REGION" SAFEEDGE_OPCUA_PORT="$PORT" \
    SAFEEDGE_OPCUA_TRUSTLIST="$trustlist" SAFEEDGE_OPCUA_INSECURE="$insecure" \
    "$UA_BUILD/safeedge-opcua" >"$WORK/server.log" 2>&1 &
  sleep 3
}

stop_server() {
  pkill -f "safeedge-opcua" 2>/dev/null
  sleep 1
}

# Exits 0 when the probe got a session, non-zero when it did not.
try_connect() {
  local log="$1"
  shift
  "$UA_BUILD/safeedge-opcua-probe" --endpoint "opc.tcp://127.0.0.1:$PORT" \
    --seconds 3 "$@" >"$log" 2>&1
  grep -q "^connected to" "$log"
}

mkdir -p "$OUT" "$WORK/trust" "$WORK/trusted-id" "$WORK/stranger-id"

{
  echo "OPC UA client authentication: does the trust list keep anyone out?"
  echo "=================================================================="
  echo
  echo "One server, two clients, and one configuration that is refused before"
  echo "any client gets the chance."
  echo

  start_runtime

  # Two identities. Only one of them will be named in the trust list, and both
  # are created before the server starts -- which is the whole point: a client
  # that mints a fresh certificate on every run is one a trust list can never
  # contain, and that is what this probe used to do.
  "$UA_BUILD/safeedge-opcua-probe" --write-identity "$WORK/trusted-id" \
    >"$WORK/id1.log" 2>&1
  "$UA_BUILD/safeedge-opcua-probe" --write-identity "$WORK/stranger-id" \
    >"$WORK/id2.log" 2>&1
  cp "$WORK/trusted-id/client.der" "$WORK/trust/"

  echo "Trust list holds $(ls -1 "$WORK/trust" | wc -l) certificate; a second identity was"
  echo "created and deliberately left out of it."
  echo
  # ---------------------------------------------------------------------
  echo "1. Trust list enforced"
  echo "----------------------"
  start_server "$WORK/trust"

  if grep -q "client certificates are checked against the trust list" "$WORK/server.log"; then
    posture=enforcing
  else
    posture=other
  fi
  check "server reports the posture it is in" "enforcing" "$posture"

  if try_connect "$WORK/trusted.log" --cert "$WORK/trusted-id/client.der" \
      --key "$WORK/trusted-id/client-key.der"; then
    trusted=connected
  else
    trusted=refused
  fi
  check "client whose certificate is in the trust list" "connected" "$trusted"

  if try_connect "$WORK/stranger.log" --cert "$WORK/stranger-id/client.der" \
      --key "$WORK/stranger-id/client-key.der"; then
    stranger=connected
  else
    stranger=refused
  fi
  check "client whose certificate is not" "refused" "$stranger"

  # The reason matters as much as the outcome: a client refused because the
  # server was not listening would look identical above. This greps for the
  # specific status code rather than the word "certificate", which also appears
  # in the startup line and would make the check unfailable.
  if grep -q "BadCertificateUntrusted" "$WORK/server.log"; then
    reason=BadCertificateUntrusted
  else
    reason=none
  fi
  check "and refused it as untrusted, specifically" "BadCertificateUntrusted" "$reason"
  stop_server
  echo
  # ---------------------------------------------------------------------
  echo "2. No trust list: the same stranger is welcome"
  echo "----------------------------------------------"
  start_server ""

  if try_connect "$WORK/stranger2.log" --cert "$WORK/stranger-id/client.der" \
      --key "$WORK/stranger-id/client-key.der"; then
    default_stranger=connected
  else
    default_stranger=refused
  fi
  # Not a bug -- the default posture protects the channel, not the door. The
  # point of running it is that the two configurations are distinguishable only
  # here, never from a client that happens to be trusted anyway.
  check "same certificate, no trust list configured" "connected" "$default_stranger"

  if grep -q "any client certificate is accepted" "$WORK/server.log"; then
    warned=warned
  else
    warned=silent
  fi
  check "server warned that anyone may connect" "warned" "$warned"

  # The same status code must NOT appear here, or the check above would be
  # passing on something unrelated to the trust list.
  if grep -q "BadCertificateUntrusted" "$WORK/server.log"; then
    untrusted=present
  else
    untrusted=absent
  fi
  check "no untrusted-certificate rejection without a list" "absent" "$untrusted"
  stop_server
  echo

  # ---------------------------------------------------------------------
  echo "3. Trust list plus a plaintext endpoint: refused outright"
  echo "---------------------------------------------------------"
  start_server "$WORK/trust" 1

  if pgrep -f "safeedge-opcua" >/dev/null; then
    started=running
  else
    started=refused
  fi
  # SecurityPolicy#None needs no certificate, so an endpoint offering it is a
  # way around the trust list rather than a fallback beside it. Every client
  # excluded above would connect to the other endpoint instead, and the server
  # would go on reporting that it authenticates clients.
  check "server with both refuses to start" "refused" "$started"

  if grep -q "way around the trust list" "$WORK/server.log"; then
    said=explained
  else
    said=silent
  fi
  check "and says why" "explained" "$said"
  echo
  echo "What this does and does not establish"
  echo "-------------------------------------"
  echo "The trust list authenticates the client APPLICATION, at the"
  echo "SecureChannel, before a session exists. That is real access control and"
  echo "is what an OPC UA deployment normally relies on."
  echo
  echo "It says nothing about WHICH USER is behind that application: anonymous"
  echo "user tokens are still accepted. For a read-only diagnostic view of a"
  echo "safety runtime there is nothing to write and nothing secret, so that is"
  echo "defensible -- and it stops being defensible the moment anything here is"
  echo "writable, which is why the address space has no writable node."
  echo
  echo "One thing to expect in the server log: open62541 warns that x509 user"
  echo "token authentication is configured without an encrypting policy. It is"
  echo "emitted while the configuration is being built, before the unencrypted"
  echo "endpoints are dropped, and it is not true of the server that ends up"
  echo "running -- the endpoint count in the posture line is the state that"
  echo "holds. A warning that was true for a moment is still a warning someone"
  echo "has to reason about at three in the morning."
  echo
  echo "The clients here also still accept whatever certificate the server"
  echo "presents. Authentication in this direction is what section 1 measures;"
  echo "the other direction needs a trust list on the client, and the probe"
  echo "prints a warning on every run saying it does not have one."
  echo
  if [ "$FAILURES" -eq 0 ]; then
    echo "All checks held."
  else
    echo "$FAILURES check(s) did not hold."
  fi
} | tee "$OUT/opcua-client-authentication.txt"

exit "$FAILURES"
