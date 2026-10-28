#!/bin/bash
# ---------------------------------------------------------------------------
# aes67-stream-watchdog.sh - keep the Salland1 FLAC pipeline producing.
#
# ffmpeg (-reconnect) can HANG on a decoder/CRC error instead of reconnecting,
# leaving aes67-tx --raw with an empty pipe (0 packets out).  systemd Restart=always
# only re-spawns when the process EXITS; a hung ffmpeg never exits.  This watchdog
# watches the output multicast group for packets and, if it stays silent too long,
# restarts the service (which kills the stuck pipeline and re-spawns it, so ffmpeg
# reconnects).  It also recovers from a network/switch drop.
#
# install as a systemd unit (see aes67-stream-watchdog.service) with Restart=always.
# Requires  tcpdump (or tshark) and root.
# ---------------------------------------------------------------------------

SVC="aes67-salland1-hd.service"
DST="239.69.100.2"
PORT=5004
CAPTURE=6        # seconds tcpdump waits for one matching packet
BAD_LIMIT=3      # restart after this many silent windows (~24 s)

bad=0
while true; do
    # -p: capture without promiscuous mode.  Verified (2026-10-28) that this
    # still sees the multicast output group, and it stops tcpdump from
    # toggling promiscuous mode on the shared production interface every few
    # seconds (~46 enter/leave events per minute in the kernel log before this
    # change), which is both log noise and a plausible source of brief RX gaps
    # for the AES67 senders on the same NIC.
    cnt=$(timeout "$CAPTURE" tcpdump -p -i eno1 -nn -c 1 -q "dst host $DST and udp port $PORT" 2>/dev/null | wc -l)
    if [ "$cnt" -gt 0 ]; then
        bad=0                       # healthy: a packet arrived
    else
        bad=$((bad+1))
        echo "$(date '+%F %T'): $DST silent (${bad}/${BAD_LIMIT})" >&2
    fi
    if [ "$bad" -ge "$BAD_LIMIT" ]; then
        echo "$(date '+%F %T'): $DST silent for ${BAD_LIMIT} windows (~$((BAD_LIMIT*CAPTURE))s) - restarting $SVC" >&2
        systemctl restart "$SVC"
        bad=0
    fi
    sleep 2
done
