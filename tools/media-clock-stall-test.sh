#!/bin/bash
# ---------------------------------------------------------------------------
# media-clock-stall-test.sh - reproduce the "media clock off PHC" detector on
# a scratch multicast group, without touching the production flows.
#
# Why
# ---
# Until v1.7.0 the raw path restarted its emit schedule whenever the process
# was late, but it did not advance the media clock.  A descheduled process (a
# real-time DAB host under load, an NFS stall, ...) therefore left the media
# clock permanently behind the PHC; a quarter of a second later the PTP-step
# detector saw that offset, logged "large PTP step detected" and fired a
# disruptive hard reseed - even though ptp4l itself never moved.  The relay
# path has the same symptom for a different reason: its media clock advances
# on *received* packets, so a receive gap leaves it behind and the following
# burst leaves it ahead.
#
# Since v1.7.1 the raw path catches up instead: a stall shorter than
# MAX_CATCHUP_S (100 ms) is absorbed by emitting the backlog immediately, so
# the media clock stays locked to the PHC and the RTP timestamps stay
# continuous.  A longer stall is resynchronised with one bounded jump
# ("media clock resynced"), and only an offset that survives that is
# hard-reseeded.
#
# What it does
# ------------
# 1. starts a private `aes67-tx --raw` sender on a scratch group (a sine wave
#    through ffmpeg, so no network source is needed);
# 2. starts a private relay of that group onto a second scratch group;
# 3. injects SIGSTOP/SIGCONT stalls into the sender, and into the relay;
# 4. reports, per stall, how many hard reseeds / bounded resyncs were logged
#    and what the last RTP-vs-PHC offset was.
#
# Requirements: Linux, root (PTP hardware clock), ffmpeg, tcpdump not needed.
# Run it on the host that owns the PTP slave, because the PHC must be real.
#
# Usage:
#   sudo ./tools/media-clock-stall-test.sh
#   sudo BIN=/tmp/aes67-tx-new IFACE=eno1 ./tools/media-clock-stall-test.sh
# ---------------------------------------------------------------------------
set -u

BIN=${BIN:-/usr/local/bin/aes67-tx}
IFACE=${IFACE:-eno1}
GRP_RAW=${GRP_RAW:-239.69.100.250}
GRP_RELAY=${GRP_RELAY:-239.69.100.251}
PORT=${PORT:-5004}
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
echo "sender : $GRP_RAW  relay: $GRP_RELAY  iface: $IFACE"
echo

: >"$LOG_RAW"
: >"$LOG_RELAY"

# --- start the two private test senders -----------------------------------
setsid bash -c "ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 -ar 48000 -f s24le - \
    | exec $BIN --raw -k 0xDEADBEE1 -a $GRP_RAW -p $PORT -f $IFACE \
        -b 24 -C 2 -n 48 -J 1024 -L 200 -t 96 -l 8 -v" >>"$LOG_RAW" 2>&1 &
setsid "$BIN" -A "$GRP_RAW" -P "$PORT" -a "$GRP_RELAY" -p "$PORT" \
    -f "$IFACE" -k 0xDEADBEE2 -t 96 -l 8 -v >>"$LOG_RELAY" 2>&1 &
sleep 1
RAW_PID=$(pgrep -f "^$BIN --raw -k 0xDEADBEE1" | head -1)
RELAY_PID=$(pgrep -f "^$BIN -A $GRP_RAW" | head -1)
if [ -z "$RAW_PID" ] || [ -z "$RELAY_PID" ]; then
    echo "could not start the test senders" >&2
    exit 1
fi
echo "raw pid $RAW_PID, relay pid $RELAY_PID"

# --- wait for the steady state (buffer at target, offset small) ------------
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
echo "steady state: $(grep -o 'fill=[^ ]* target=[^ ]*.*' "$LOG_RAW" | tail -1)"
echo

count_reseed() { # logfile
    grep -c "hard reseed" "$1" 2>/dev/null || true
}
count_resync() { # logfile
    grep -c "media clock resynced" "$1" 2>/dev/null || true
}
last_offset() { # logfile
    grep -o "offset_sample=[^ ]* ([^)]*)" "$1" 2>/dev/null | tail -1
}

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

echo "--- sender stalls (a v1.7.1 sender catches up, a v1.7.0 sender reseeds) ---"
stall_raw 0.010 "10 ms stall";  expect "10 ms stall"  "$GR" "$GS" 0 0
stall_raw 0.030 "30 ms stall";  expect "30 ms stall"  "$GR" "$GS" 0 0
stall_raw 0.050 "50 ms stall";  expect "50 ms stall"  "$GR" "$GS" 0 0
stall_raw 0.150 "150 ms stall (> catch-up)"; expect "150 ms stall" "$GR" "$GS" 1 1

echo
echo "--- relay stalls (informational; relay anchoring on the PHC is still open) ---"
for d in 0.030 0.050; do
    r0=$(count_reseed "$LOG_RELAY")
    kill -STOP "$RELAY_PID" 2>/dev/null
    sleep "$d"
    kill -CONT "$RELAY_PID" 2>/dev/null
    sleep 1.5
    r1=$(count_reseed "$LOG_RELAY")
    printf "  %-24s reseeds=%s\n" "relay stall ${d#0.}00 ms" "$((r1-r0))"
done

echo
echo "--- last log lines ---"
echo "raw  : $(tail -1 "$LOG_RAW")"
echo "relay: $(tail -1 "$LOG_RELAY")"
[ -s "$LOG_RELAY" ] && grep "hard reseed" "$LOG_RELAY" | tail -3 | sed "s/^/relay: /"

echo
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
else
    echo "RESULT: FAIL"
fi
exit "$fail"
