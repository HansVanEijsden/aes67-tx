#!/bin/bash
# ---------------------------------------------------------------------------
# ptp-watchdog.sh - keep the PTP slave clock sane across grandmaster/switch reboots.
#
# A strict AES67/Dante receiver requires a sender's media clock to reference the PTP
# grandmaster.  Here the grandmaster is the Dante bridge/VertoMX, a *free-running*
# class-248 clock (no external clock, not PTP timescale).  When it reboots its clock
# resets to an arbitrary value (we have seen ~4 hours off).  ptp4l (slave-only) must
# re-synchronise; usually it does, but occasionally it gets stuck holding the old
# (wrong) time, and then every AES67 source is off by a huge offset until ptp4l is
# restarted.  This script watches the PTP TIME_STATUS_NP and restarts ptp4l when the
# clock offset stays wrong too long.  It is deliberately conservative: it only acts
# when the offset is large AND the master is present AND it persists for ~60 s.
#
# install as a systemd unit (see ptp-watchdog.service) with Restart=always so it
# survives reboots and restarts itself.
#
# Requires: pmc (linuxptp) and systemctl, run as root.
# ---------------------------------------------------------------------------

PTP_SVC="ptp4l-slave-eno1.service"   # the slave-only ptp4l unit
CHECK_EVERY=10                        # seconds between status checks
BAD_LIMIT=6                           # restart after this many consecutive bad checks (~60 s)
OFFSET_NS_LIMIT=1000000              # 1 ms: normal offset is ~1-12 us, so > 1 ms = not synced

bad=0
while true; do
    out=$(pmc -u -b 0 "GET TIME_STATUS_NP" 2>/dev/null)
    gm=$(printf '%s\n' "$out" | awk '/gmPresent/{print $2}')
    off=$(printf '%s\n' "$out" | awk '/master_offset/{print $2}')
    # master_offset can be negative; normalise to a non-negative integer
    off="${off#-}"
    case "$off" in ''|*[!0-9]*) off=0;; esac

    if [ "$gm" = "true" ] && [ "$off" -gt "$OFFSET_NS_LIMIT" ] 2>/dev/null; then
        bad=$((bad+1))
        echo "$(date '+%F %T'): PTP offset ${off} ns (bad ${bad}/${BAD_LIMIT}); gm present" >&2
    else
        bad=0   # synced, or no master yet (e.g. power off / switch down) - do nothing
    fi

    if [ "$bad" -ge "$BAD_LIMIT" ]; then
        echo "$(date '+%F %T'): PTP clock stuck (offset ${off} ns) - restarting ${PTP_SVC}" >&2
        systemctl restart "$PTP_SVC"
        bad=0
    fi

    sleep "$CHECK_EVERY"
done
