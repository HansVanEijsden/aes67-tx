#!/bin/sh
# Example startup script for aes67-tx.
# This is the human-friendly way to run it; for production prefer the
# systemd unit (aes67-tx.service) so it survives reboots and restarts.
#
# Adjust the variables below to match your setup, then:
#   chmod +x startup.sh && ./startup.sh

set -e

# --- configuration --------------------------------------------------------
BIN="/usr/local/bin/aes67-tx"   # path to the installed binary (or ./aes67-tx)
INPUT_ADDR="239.192.19.137"     # existing AES67/RTP source (or use INPUT_SDP)
INPUT_PORT="5004"
INPUT_SDP=""                    # e.g. "/etc/aes67/source.sdp" ; if set, overrides addr/port

OUTPUT_ADDR="239.69.100.1"      # new AES67 output group
OUTPUT_PORT="5004"
IFACE="eth0"                    # interface carrying the output multicast
PTP_DEV="/dev/ptp0"             # the PTP-slaved hardware clock
PT="96"                         # output RTP payload type
RESTAMP="no"                    # "yes" to re-stamp from the PTP clock
RATE="48000"

# --- build command --------------------------------------------------------
set -- -a "$OUTPUT_ADDR" -p "$OUTPUT_PORT" -f "$IFACE" -d "$PTP_DEV" -t "$PT"
if [ -n "$INPUT_SDP" ]; then
    set -- "$@" -S "$INPUT_SDP"
else
    set -- "$@" -A "$INPUT_ADDR" -P "$INPUT_PORT"
fi
[ "$RESTAMP" = "yes" ] && set -- "$@" -R -B "$RATE"
set -- "$@" -v

echo "Starting: sudo $BIN $*"
exec sudo "$BIN" "$@"
