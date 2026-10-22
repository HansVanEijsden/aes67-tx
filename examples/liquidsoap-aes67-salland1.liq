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
# and the adaptive resampling; target 1.0 s, 3 s ring capacity.
output.external(
  %wav(header=false, samplerate=48000, channels=2, samplesize=24),
  "/usr/local/bin/aes67-tx --raw -k 0x6a95A511 -a 239.69.100.2 -p 5004 -f eno1 -b 24 -C 2 -n 48 -J 3072 -L 1000 -t 96 -l 8 -v",
  s,
  self_sync=true)
