settings.init.allow_root := true
settings.log.level := 3
settings.log.file := false
settings.log.stdout := true

# Salland1 FLAC (lossless) as a normal internet radio stream.  Keep the
# Liquidsoap-side buffer small (0.5 s) so the total end-to-end latency stays
# around 1.5 s; aes67-tx provides the larger, drift-corrected ring.
s = mksafe(input.http("https://radio.salland1.nl/salland1-flac",
                      max_buffer=0.5, self_sync=false))

# Raw 24-bit PCM on the child's stdin.  aes67-tx 1.6.0 does the PTP stamping
# and the adaptive resampling.  The program feed does not need to be live, so
# the buffer is large: -L 10000 (10 s target) with -J 30720 (30 s capacity).
# Upstream stalls below ~10 s are then played out of the buffer with no gap at
# all, 10-20 s stalls cost only the excess, and -U 30 keeps the pipeline alive
# through a stall of up to 30 s.  This adds ~10 s of program delay.
#
# -U 30: the Salland1 uplink is a known lossy link (packet loss, reordering),
# so a TCP stall can freeze input.http's blocking read for tens of seconds.
# With -U 10 every such stall killed the pipeline (aes67-tx exits -> Liquidsoap
# dies on the broken pipe -> systemd restart, ~17 s of silence per event).
# -U 30 lets the chain ride out a stall of up to 30 s: the audio gap equals the
# uplink stall instead of the stall plus a restart.
output.external(
  %wav(header=false, samplerate=48000, channels=2, samplesize=24),
  "/usr/local/bin/aes67-tx --raw -k 0x6a95A511 -a 239.69.100.2 -p 5004 -f eno1 -b 24 -C 2 -n 48 -J 30720 -L 10000 -U 30 -t 96 -l 8 -v",
  s,
  self_sync=true)
