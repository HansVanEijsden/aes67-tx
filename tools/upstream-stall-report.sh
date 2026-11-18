#!/bin/bash
# ---------------------------------------------------------------------------
# upstream-stall-report.sh - summarise Salland1 upstream stalls from the journal
#
# The Salland1 FLAC feed arrives over a lossy link (known packet loss on the
# Wijhe connection, Butt -> Icecast on the VPS).  Every time the feed stops
# delivering PCM, aes67-tx drains its input ring and then transmits silence
# (zero packets) until the feed returns; if that lasts longer than -U seconds
# it exits so systemd restarts the pipeline.
#
# This script reads the per-second verbose log of the aes67-tx child
# (`fill=...ms ... underruns=N`) and reports, per day:
#   - the number of stall events,
#   - the total inserted silence (sum of skipped media-clock slots),
#   - the longest single stall,
#   - a duration histogram, so we can decide whether the -L target buffer
#     (currently 10 s) should be raised (e.g. 20 s) or not,
#   - how often the buffer was below 1 s (the "fragile" window after a stall),
#   - the number of pipeline restarts.
#
# Usage (on the AES67 host):
#   aes67-stall-report.sh          # last 7 days
#   aes67-stall-report.sh 3        # last 3 days
# ---------------------------------------------------------------------------
set -u
UNIT=${UNIT:-aes67-salland1-hd}
DAYS=${1:-7}
SINCE="${DAYS} days ago"

command -v journalctl >/dev/null 2>&1 || { echo "journalctl not found" >&2; exit 1; }

echo "Salland1 upstream-stall report — last ${DAYS} day(s), unit ${UNIT}"
echo "-------------------------------------------------------------"
journalctl -u "$UNIT" --since "$SINCE" --no-pager 2>/dev/null | awk '
  BEGIN { prev = 0; in_stall = 0; gap = 0 }
  /fill=[0-9.]+ms/ && /underruns=[0-9]+/ {
      day = $1 " " $2
      match($0, /fill=[0-9.]+ms/);  fill = substr($0, RSTART + 5, RLENGTH - 7) + 0
      match($0, /underruns=[0-9]+/); u    = substr($0, RSTART + 10, RLENGTH - 10) + 0

      if (u > prev) {
          if (!in_stall) {
          in_stall = 1; stall_day = day; n_events++
          if (!(day in day_events)) day_order[++nd] = day
          day_events[day]++
      }
          d = u - prev
          gap += d; total_gap += d; day_gap[day] += d
      } else if (in_stall) {
          in_stall = 0
          if (gap > max_gap) { max_gap = gap; max_day = stall_day }
          if (gap < 1000) b1++; else if (gap < 5000) b2++; else if (gap < 10000) b3++
          else if (gap < 20000) b4++; else if (gap < 30000) b5++; else b6++
          gap = 0
      }
      if (fill < 1000) fragile++
      samples++
      prev = u
  }
  END {
      if (in_stall) {   # stall still running when the window ended
          if (gap > max_gap) { max_gap = gap; max_day = stall_day }
          if (gap < 1000) b1++; else if (gap < 5000) b2++; else if (gap < 10000) b3++
          else if (gap < 20000) b4++; else if (gap < 30000) b5++; else b6++
      }
      printf "stall events          : %d\n", n_events
      printf "inserted silence      : %.1f s total\n", total_gap / 1000
      printf "longest single stall  : %.1f s%s\n", max_gap / 1000, (max_gap ? " (started " max_day ")" : "")
      printf "duration histogram    : <1s=%d  1-5s=%d  5-10s=%d  10-20s=%d  20-30s=%d  >30s=%d\n", b1, b2, b3, b4, b5, b6
      printf "buffer samples        : %d, of which below 1 s: %d (%.1f %%)\n", samples, fragile, samples ? 100 * fragile / samples : 0
      printf "\nper day:\n"
      for (i = 1; i <= nd; i++) {
          d = day_order[i]
          printf "  %-8s events=%-3d inserted silence=%.1f s\n", d, day_events[d], day_gap[d] / 1000
      }
  }'
echo "-------------------------------------------------------------"
printf "pipeline restarts     : %s\n" "$(journalctl -u "$UNIT" --since "$SINCE" --no-pager 2>/dev/null | grep -c 'Started aes67-salland1-hd')"
printf "underrun exits        : %s\n" "$(journalctl -u "$UNIT" --since "$SINCE" --no-pager 2>/dev/null | grep -c 'raw input underrun for')"
