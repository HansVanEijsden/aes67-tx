#!/bin/bash
# ---------------------------------------------------------------------------
# media-clock-stall-test.sh - exercise the media-clock recovery paths of
# aes67-tx on scratch multicast groups, without touching the production flows.
#
# What it covers
# --------------
# 1. Sender stalls (--raw path): since v1.7.1 a stall shorter than
#    MAX_CATCHUP_S (100 ms) is caught up with continuous timestamps and must
#    NOT produce a hard reseed; a longer lag is resynchronised with one bounded
#    jump and any residual offset is reseeded.  Before v1.7.1 every stall was
#    logged as a "PTP step" and reseeded - the bug this test pins down.
# 2. Relay stalls and arrival gaps: the relay must not reseed when its own
#    emitter is merely behind schedule, and (with --relay-buffer) it must keep
#    its media clock anchored on the PHC while the jitter buffer absorbs
#    arrival jitter.  With RELAY_BUFFER=0 the legacy arrival-paced relay is
#    tested instead.
#
# The test never uses the production groups (239.69.100.1/.2): it publishes a
# sine wave on scratch groups and relays that onto a second scratch group.
#
# Requirements: Linux, root (PTP hardware clock), ffmpeg, tcpdump.
# Run it on the host that owns the PTP slave, so the PHC is real.
#
# Usage:
#   sudo ./tools/media-clock-stall-test.sh
#   sudo BIN=/tmp/aes67-tx-new RELAY_BUFFER=0 ./tools/media-clock-stall-test.sh
# ---------------------------------------------------------------------------
set -u

BIN=${BIN:-/usr/local/bin/aes67-tx}
IFACE=${IFACE:-eno1}
GRP_RAW=${GRP_RAW:-239.69.100.250}
GRP_RELAY=${GRP_RELAY:-239.69.100.251}
PORT=${PORT:-5004}
RELAY_BUFFER=${RELAY_BUFFER:-4}      # ms; 0 = legacy arrival-paced relay
LOG_RAW=${LOG_RAW:-/tmp/aes67-stall-raw.log}
LOG_RELAY=${LOG_RELAY:-/tmp/aes67-stall-relay.log}

RAW_PID=""
RELAY_PID=""
fail=0

cleanup() {
    [ -n "$RAW_PID" ] && kill "$RAW_PID" 2>/dev/null
    [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null
    sleep 0.5
    # bracket trick: the pattern must not match this script's own cmdline
    pkill -f "lavfi -i sine=frequenc[y]=440" 2>/dev/null
}
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || { echo "must run as root (PTP hardware clock)" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found" >&2; exit 1; }
command -v tcpdump >/dev/null 2>&1 || { echo "tcpdump not found" >&2; exit 1; }
[ -x "$BIN" ] || { echo "aes67-tx binary not found: $BIN" >&2; exit 1; }

for g in "$GRP_RAW" "$GRP_RELAY"; do
    case "$g" in
        239.69.100.1|239.69.100.2)
            echo "refusing to run on production group $g" >&2
            exit 1
            ;;
    esac
done

echo "binary : $BIN"
echo "sender : $GRP_RAW  relay: $GRP_RELAY  iface: $IFACE  relay buffer: ${RELAY_BUFFER} ms"
echo

: >"$LOG_RAW"
: >"$LOG_RELAY"

# --- start the two private test senders -----------------------------------
setsid bash -c "ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 -ar 48000 -f s24le - \
    | exec $BIN --raw -k 0xDEADBEE1 -a $GRP_RAW -p $PORT -f $IFACE \
        -b 24 -C 2 -n 48 -J 1024 -L 200 -t 96 -l 8 -v" >>"$LOG_RAW" 2>&1 &
RELAY_ARGS="-A $GRP_RAW -P $PORT -a $GRP_RELAY -p $PORT -f $IFACE -k 0xDEADBEE2 -t 96 -l 8 -v"
[ "$RELAY_BUFFER" -gt 0 ] && RELAY_ARGS="$RELAY_ARGS --relay-buffer $RELAY_BUFFER"
setsid "$BIN" $RELAY_ARGS >>"$LOG_RELAY" 2>&1 &
sleep 1
RAW_PID=$(pgrep -f "^$BIN --raw -k 0xDEADBEE1" | head -1)
RELAY_PID=$(pgrep -f "^$BIN -A $GRP_RAW" | head -1)
if [ -z "$RAW_PID" ] || [ -z "$RELAY_PID" ]; then
    echo "could not start the test senders" >&2
    exit 1
fi
echo "raw pid $RAW_PID, relay pid $RELAY_PID"

# --- wait for the steady state -------------------------------------------
for _ in $(seq 1 40); do
    grep -q "offset_sample" "$LOG_RAW" && break
    sleep 0.5
done
sleep 2
if ! grep -q "offset_sample" "$LOG_RAW"; then
    echo "raw sender never reached steady state:" >&2
    tail -5 "$LOG_RAW" >&2
    exit 1
fi
echo "raw steady: $(grep -o 'fill=[^ ]* target=[^ ]*.*' "$LOG_RAW" | tail -1)"
echo "relay steady: $(tail -1 "$LOG_RELAY")"
if [ "$RELAY_BUFFER" -gt 0 ]; then
    rl=$(tail -1 "$LOG_RELAY")
    fl=$(printf '%s' "$rl" | sed -n 's/.*fill=\([0-9]*\)\/[0-9]* ms.*/\1/p')
    dr=$(printf '%s' "$rl" | sed -n 's/.*dropped=\([0-9]*\).*/\1/p')
    un=$(printf '%s' "$rl" | sed -n 's/.*underruns=\([0-9]*\).*/\1/p')
    if [ "${fl:-0}" -ge 2 ] && [ "${dr:-1}" -eq 0 ] && [ "${un:-1}" -eq 0 ]; then
        echo "  OK   relay buffer settled (fill ${fl} ms, no drops/underruns)"
    else
        echo "  FAIL relay buffer did not settle (fill ${fl} ms, dropped ${dr}, underruns ${un})"
        fail=1
    fi
fi
echo

count_reseed() { grep -c "hard reseed" "$1" 2>/dev/null || true; }
count_resync() { grep -c "media clock resynced" "$1" 2>/dev/null || true; }
count_under()  { grep -c "relay underrun" "$1" 2>/dev/null || true; }
last_offset()  { grep -o "offset_sample=[^ ]* ([^)]*)" "$1" 2>/dev/null | tail -1; }

GR=0; GS=0   # result of the last stall_raw (reseeds / resyncs added)
stall_raw() { # delay-seconds  label
    local d=$1 label=$2
    local r0 rs0 r1 rs1
    r0=$(count_reseed "$LOG_RAW"); rs0=$(count_resync "$LOG_RAW")
    kill -STOP "$RAW_PID" 2>/dev/null
    sleep "$d"
    kill -CONT "$RAW_PID" 2>/dev/null
    sleep 1.5
    r1=$(count_reseed "$LOG_RAW"); rs1=$(count_resync "$LOG_RAW")
    GR=$((r1-r0)); GS=$((rs1-rs0))
    printf "%-30s reseeds=%-2s resyncs=%-2s offset=%s\n" \
        "$label" "$GR" "$GS" "$(last_offset "$LOG_RAW")"
}

expect() { # label got_reseeds got_resyncs exp_reseeds exp_resyncs
    local label=$1 gr=$2 gs=$3 er=$4 es=$5
    if [ "$gr" -eq "$er" ] && [ "$gs" -eq "$es" ]; then
        printf "  OK   %-26s (expected reseeds=%s resyncs=%s)\n" "$label" "$er" "$es"
    else
        printf "  FAIL %-26s got reseeds=%s resyncs=%s, expected reseeds=%s resyncs=%s\n" \
            "$label" "$gr" "$gs" "$er" "$es"
        fail=1
    fi
}

# packet-interval stats on the relay output group: "pps min_ms max_ms"
intervals() { # group
    local g=$1 f=/tmp/aes67-stall-cap.txt
    timeout 3 tcpdump -p -i "$IFACE" -nn -tt -q "dst host $g and udp port $PORT" \
        >"$f" 2>/dev/null
    awk '{ t=$1+0; if (t<1e9) next; if (pf==0) { pf=t; p=t; next }
           d=(t-p)*1000; n++; s+=d; if (n==1||d<mn) mn=d; if (d>mx) mx=d; p=t }
     END { span=p-pf; if (n<1 || span<=0) { print "0 0 0"; exit }
           printf "%.0f %.3f %.3f\n", (n+1)/span, mn, mx }' "$f" 2>/dev/null
    rm -f "$f"
}

echo "--- sender stalls (v1.7.1+ catches up, v1.7.0 reseeded) ---"
stall_raw 0.010 "10 ms stall";  expect "10 ms stall"  "$GR" "$GS" 0 0
stall_raw 0.030 "30 ms stall";  expect "30 ms stall"  "$GR" "$GS" 0 0
stall_raw 0.050 "50 ms stall";  expect "50 ms stall"  "$GR" "$GS" 0 0
stall_raw 0.150 "150 ms stall (> catch-up)"; expect "150 ms stall" "$GR" "$GS" 1 1

echo
echo "--- relay ---"
if [ "$RELAY_BUFFER" -gt 0 ]; then
    # source gap: the jitter buffer absorbs what it can, the media clock must
    # stay on the PHC (no reseed, no fabricated audio)
    r0=$(count_reseed "$LOG_RELAY"); u0=$(count_under "$LOG_RELAY")
    stall_raw 0.030 "source gap 30 ms" >/dev/null
    r1=$(count_reseed "$LOG_RELAY"); u1=$(count_under "$LOG_RELAY")
    printf "  source gap 30 ms            relay reseeds=%s new underruns=%s\n" \
        "$((r1-r0))" "$((u1-u0))"
    if [ "$((r1-r0))" -eq 0 ]; then
        printf "  OK   relay kept its clock through the source gap\n"
    else
        printf "  FAIL relay reseeded on a source gap\n"; fail=1
    fi
fi

# relay process stall: catch-up, must not reseed
r0=$(count_reseed "$LOG_RELAY")
kill -STOP "$RELAY_PID" 2>/dev/null; sleep 0.030; kill -CONT "$RELAY_PID" 2>/dev/null
sleep 1.5
r1=$(count_reseed "$LOG_RELAY")
printf "  relay stall 30 ms           relay reseeds=%s\n" "$((r1-r0))"
if [ "$((r1-r0))" -eq 0 ]; then
    printf "  OK   relay caught up after its own stall\n"
else
    printf "  FAIL relay reseeded after its own stall\n"; fail=1
fi

set -- $(intervals "$GRP_RELAY")
pps=${1:-0}; mn=${2:-0}; mx=${3:-0}
printf "  relay output: %s pps, interval min %s ms, max %s ms\n" "$pps" "$mn" "$mx"
if [ "${pps%.*}" -ge 990 ] && [ "${pps%.*}" -le 1010 ] && awk -v m="$mx" 'BEGIN{exit !(m<5)}'; then
    printf "  OK   relay output is evenly paced\n"
else
    printf "  FAIL relay output pacing looks wrong\n"; fail=1
fi

echo
echo "--- last log lines ---"
echo "raw  : $(tail -1 "$LOG_RAW")"
echo "relay: $(tail -1 "$LOG_RELAY")"
grep "hard reseed" "$LOG_RELAY" 2>/dev/null | tail -3 | sed "s/^/relay: /"

echo
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
else
    echo "RESULT: FAIL"
fi
exit "$fail"
