# aes67-tx — operational status

**Last verified:** 2026-10-28 (Europe/Amsterdam)  
**Current version:** 1.7.0  
**Repository:** public — this file deliberately contains no secrets, credentials, host/IP addresses, PTP grandmaster identifiers, stream URLs or private network details. Operational runbooks remain in the private infrastructure documentation.

## Current production state

One Linux host acts as a PTP slave of the Dante/AES67 grandmaster and publishes two AES67 flows:

| Flow | Source | Mode | Status |
|---|---|---|---|
| 1Zwolle HD | Stereo Tool AES67 / LiveWire | RTP relay, re-stamped from the PTP hardware clock | `1.7.0`, up since 2026-10-24, no service restarts |
| Salland1 HD | Salland1 HTTP/FLAC stream | Liquidsoap → raw 24-bit PCM → `aes67-tx --raw` | `1.7.0`, current run up since 2026-10-27 after two input-stall restarts |

The Salland1 chain:

- Liquidsoap fetches the lossless FLAC stream with `input.http` and writes raw 24-bit PCM via `output.external`.
- `aes67-tx --raw` options in production: `-J 3072` (3 s ring capacity), `-L 1000` (1 s target), `-U 10` (exit after 10 s of underrun).
- The service runs as root in a dedicated systemd unit because `aes67-tx` must open the PTP hardware clock and the multicast socket.
- The existing `aes67-stream-watchdog` monitors packet presence; `-U 10` covers the separate case where the decoder stays alive but stops delivering PCM.

The 1Zwolle HD relay and the Salland1 receiver share the same binary and PTP PLL. The 1Zwolle **webstream** Liquidsoap service is a separate production chain and was not touched during this work.

GitHub releases exist for **v1.5.0 through v1.7.0**; v1.7.0 is the latest. The releases provide source archives only (no binary assets). Documentation-only commits use `[no release]` and do not trigger a new release.

## Current health

Latest measured production values:

- Salland1 PTP offset: ~**+0.04 ms**; 1Zwolle: ~**+0.03 ms**.
- Salland1 buffer: **1000.0 ms** (target 1000 ms), `corr` **0.0 ppm**, **0 underruns**.
- Last observed hard PTP reseed: 2026-10-28 04:16; none since then.

Measurement after ~3 h 48 min on 2026-10-24:

| Flow | Average latency | Peak | Late packets | Notes |
|---|---:|---:|---:|---|
| Salland1 HD | **0.245 ms** | 4.8 ms | 17,727 | ~0.13% of packets; PTP-paced |
| 1Zwolle HD | **0.724 ms** | 15 ms | 2,239 | ~0.016%; unbuffered relay |
| DVS reference | 2.2 / 1.7 ms | 2.3 / 2.9 ms | 0 | Windows Dante Virtual Soundcard, 5 ms latency |

Dante gives AES67 flows a fixed 2 ms latency budget; DVS can use its 5 ms setting. The remaining Late packets are short spikes above the 2 ms budget, not a structural offset.

## What works

- PTP hardware-clock slaving with `ptp4l`; Chrony keeps the **system/wall clock** independently. No `phc2sys` or `timemaster` is used.
- Media-clock **PLL/slew**: small RTP↔PTP offsets are corrected over ~30 s at a bounded rate, so timestamps stay continuous. Only genuine steps >20 ms trigger a hard reseed.
- Adaptive resampling for `--raw`: a 32-tap Kaiser-windowed sinc with a leaky, clamped PI controller holds the input ring at the target latency. This compensates the remote FLAC encoder clock against the PTP clock and prevents growing latency or zero-packet hiccups.
- ASRC anti-windup: the buffer stays at ~1 s instead of oscillating between ~864 and ~1086 ms.
- Underrun exit (`-U 10`) plus systemd restart: a stopped PCM pipe is recovered automatically instead of silently transmitting zero packets.
- Existing Dante Controller subscriptions and SDP sessions are reused; no new SDP import is needed for routine service changes.
- The SDP examples document the required PTP reference (`a=ts-refclk`); the actual grandmaster identity and domain are network-specific and therefore not recorded here.

## Decisions taken

1. **Reuse the existing multicast groups, SSRC, payload type and SDP sessions.** Service changes must not require a new Dante Controller import.
2. **Liquidsoap is the Salland1 front-end**, not ffmpeg. It provides robust HTTP reconnection/decoding; `mksafe` keeps the process alive during short input loss.
3. **Target 1 s buffer, 3 s ring capacity** for the internet-fed Salland1 flow. This is intentional latency for network robustness; it is not a Dante latency setting.
4. **PLL/slew instead of frequent hard reseeds.** Small offsets are corrected continuously; hard reseed is reserved for large PTP steps.
5. **Chrony and PTP are separate clock domains.** Chrony disciplines the wall clock; `aes67-tx` uses the PTP hardware clock directly. Do not tie the system clock to the Dante clock.
6. **Keep the 1Zwolle webstream chain isolated.** Restarting or changing it is not part of AES67 maintenance.

## Open items

1. **Large PTP steps (>20 ms)** still cause a hard reseed and therefore a short RTP-timestamp discontinuity. Events were observed on 2026-10-27 and 2026-10-28 (relay and Salland1). The cause at the grandmaster/network level has not been investigated; decide whether to widen the PLL range or change the hard-reset threshold after testing.
2. **Salland1 input stalls.** Two 10 s underrun restarts occurred on 2026-10-27. `-U 10` handled them correctly, but the upstream cause was not identified.
3. **Audio-level watchdog not implemented.** The packet watchdog and underrun timeout cover a dead pipe, but not the case where the decoder keeps sending *silent* PCM with valid packet timing.
4. **Residual Late spikes.** 1Zwolle's unbuffered relay peaks at ~15 ms; a small relay jitter buffer or a 1–2 ms timestamp lead could reduce this, but has not been implemented or tested in production.
5. **1Zwolle AES67 relay** runs the same 1.7.0 binary; its webstream Liquidsoap service remains separate and unchanged.

## Next step

Start the next workspace by re-reading this file and the README, then verify the current service/version and PTP offsets before changing anything. The first priorities are:

- observe the current 1.7.0 behaviour over a longer period and after the next PTP/network event;
- decide whether the residual Late spikes justify a small timestamp lead or relay buffer (test on a non-production group first);
- add an audio-level watchdog if silent-PCM failures matter;
- only then consider changing the hard-reset threshold or PLL parameters.
