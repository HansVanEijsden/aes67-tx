# aes67-tx — operational status

**Last verified:** 2026-11-05 16:03 (Europe/Amsterdam)  
**Current version:** 1.7.2 (deployed 2026-11-05 16:00; see the deployment section below)  
**Repository:** public — this file deliberately contains no secrets, credentials, host/IP addresses, PTP grandmaster identifiers, stream URLs or private network details. Operational runbooks remain in the private infrastructure documentation.

## Current production state

One Linux host acts as a PTP slave of the Dante/AES67 grandmaster and publishes two AES67 flows:

| Flow | Source | Mode | Status |
|---|---|---|---|
| 1Zwolle HD | Stereo Tool AES67 / LiveWire | **paced relay** (`--relay-buffer 4`), media clock anchored on the PHC | `1.7.2`, up since 2026-11-05 16:00 (deployment restart) |
| Salland1 HD | Salland1 HTTP/FLAC stream | Liquidsoap → raw 24-bit PCM → `aes67-tx --raw` | `1.7.2`, up since 2026-11-05 16:00 (deployment restart) |

The Salland1 chain:

- Liquidsoap fetches the lossless FLAC stream with `input.http` and writes raw 24-bit PCM via `output.external`.
- `aes67-tx --raw` options in production: `-J 3072` (3 s ring capacity), `-L 1000` (1 s target), `-U 10` (exit after 10 s of underrun).
- The service runs as root in a dedicated systemd unit because `aes67-tx` must open the PTP hardware clock and the multicast socket.
- The existing `aes67-stream-watchdog` monitors packet presence; `-U 10` covers the separate case where the decoder stays alive but stops delivering PCM.

The 1Zwolle HD relay and the Salland1 receiver share the same binary and PTP PLL. The 1Zwolle **webstream** Liquidsoap service is a separate production chain and was not touched during this work.

GitHub releases exist for **v1.5.0 through v1.7.1**; v1.7.1 is the latest. The releases provide source archives only (no binary assets). Documentation-only commits use `[no release]` and do not trigger a new release.

## Current health

Latest measured production values (2026-11-05 15:46):

- Salland1 RTP↔PHC offset: **−0.02…+0.02 ms**; PTP `master_offset` −9.1 µs, `offsetFromMaster` 2.6 µs.
- Salland1 buffer: **1000.0 ms** (target 1000 ms), `corr` **0.0 ppm**, **0 underruns**.
- Last hard reseed: **2026-11-04 04:20:37** (+44.7 ms, Salland1 only, no PTP event); before that none since the v1.7.1 deployment on 2026-10-28.

## Verification on 2026-10-28 (evening)

A read-only check on the production host confirmed the picture above: `aes67-tx 1.7.0` on both
flows, `ptp4l-slave-eno1` up since 2026-08-31, both watchdogs active, `portState SLAVE`,
`offsetFromMaster` −69 ns, both groups live at exactly 1000 pps (packet intervals 0.72–1.61 ms,
no gaps), Salland1 `fill` 1000.0 ms, `corr` +0.0 ppm, 0 underruns. Service restarts: relay none
since 2026-10-24; Salland1 `NRestarts=3`, two of them the `-U 10` underrun exits of 2026-10-27.

The important finding concerns the "PTP steps". Seven hard reseeds were logged since 2026-10-24,
but **none of them coincided with a PTP event**: around 2026-10-28 04:15–04:16 the `ptp4l` servo
reported rms 8 µs / max 13 µs, and `gmTimeBaseIndicator` and `lastGmPhaseChange` were 0. A
controlled test on a scratch multicast group reproduced the production log lines with local
`SIGSTOP` stalls alone: a 30 ms stall produced `+30.1 ms`, 50 ms produced `+49.4 ms`, and a 30 ms
stall of the relay (which then drains a backlog burst) produced a `+34.4`/`−25.6 ms` pair — the
same signature as 2026-10-27 21:29. `ptp4l` never moved during any of it.

Cause: the raw path restarted its emit schedule after a missed slot without advancing the media
clock, so a stall left a permanent offset that the detector then treated as a PTP step; the relay
clock advances on received packets, so network gaps and bursts modulate it directly. v1.7.1
catches up stalls ≤100 ms, resynchronises longer ones with a bounded jump, and logs the real
cause. The remaining question — what makes the host stall for 20–50 ms about once a day — is
host-level: real-time scheduling/preemption, D-state (load average ~6 while 88% idle), or the
stream watchdog's `tcpdump` promiscuous-mode flapping (46 enter/leave events per minute before
the change).

### Deployment of v1.7.1 (2026-10-28 23:57)

- Built from the released source on the host (`gcc -O2 -Wall -Wextra`, no warnings; CI green on
  both `main` and the tag). Source sha256 verified after transfer.
- The stall test was run against the exact artefact first, on scratch group 239.69.100.250:
  10/30/50 ms stalls → 0 reseeds, RTP↔PHC offset stays ≈ −0.75 ms; 150 ms → 1 bounded resync +
  1 reseed. `RESULT: PASS`.
- Binary installed atomically (`mv` over `/usr/local/bin/aes67-tx`); previous version kept as
  `/usr/local/bin/aes67-tx.1.7.0.bak` (sha256 `abded389…`).
- `aes67-stream-watchdog.sh` updated (`tcpdump -p`), previous copy kept as
  `/usr/local/bin/aes67-stream-watchdog.sh.1.7.0.bak`.
- Services restarted one at a time: relay 23:57:16, Salland1 23:57:25. Both running binaries
  verified through `sha256sum /proc/<pid>/exe`.
- Post-deployment: both groups live, Salland1 `fill` ≈ 1001 ms, `corr` +18 ppm, 0 underruns,
  RTP↔PHC offset −0.08 ms, `portState SLAVE`, `gmPresent` true; **no** reseeds/resyncs so far;
  promiscuous-mode kernel events dropped from ~45/min to **0/min**.

### Week after deployment (measured 2026-11-05 15:46)

- Relay: **one continuous run of 7 d 15 h**, 0 restarts, no reseeds at all. Both running
  processes verified as 1.7.1 (`sha256sum /proc/<pid>/exe`).
- False reseeds: **7 events in 4.5 days before → 1 in 7.6 days after**; 0 `media clock resynced`
  (i.e. no stall exceeded the 100 ms catch-up window). Promiscuous-mode kernel events stayed at
  **0/min** all week.
- The single reseed (2026-11-04 04:20:37, `+44.7 ms`) was again **not** a grandmaster step:
  `ptp4l` stayed at rms 0.1–6.3 µs / max 12.7 µs around it, `gmTimeBaseIndicator` is still 0, and
  the relay — which compares against the same PHC — logged nothing. So it is local to the
  Salland1 process. Remaining hole: a short *backward* PHC excursion (the emit loop then simply
  waits, so no forward lag is ever seen) or a preemption exactly inside the detector window is
  not caught by the catch-up. Candidate v1.7.2: log the emit lag at the moment of reseed, and
  catch up instead of reseeding when the loop itself is behind schedule.
- Salland1: two upstream underrun exits (`raw input underrun for 10 s`, 2026-11-03 09:19:58 and
  2026-11-04 22:19:04); Liquidsoap reconnected and the service restarted cleanly via `-U 10`
  plus systemd. Upstream cause still unidentified (open item 2). Current run (since 11-04
  22:19): `fill` 1000.0 ms, `corr` +0.0 ppm, 0 underruns, RTP↔PHC offset ±0.02 ms.
- PTP: `portState SLAVE`, `gmPresent` true, `master_offset` −9.1 µs and `offsetFromMaster`
  2.6 µs — noticeably noisier than the −0.3 µs / −0.07 µs of 2026-10-28, but still far inside
  the 2 ms Dante budget. System load unchanged (~5.5 on 6 cores, ~88% idle).

### Deployment of v1.7.2 (2026-11-05 16:00)

What changed since 1.7.1 (see README for the full description):

- **lag-aware reseed**: an RTP↔PHC offset that exists while the emitter itself is behind its PHC
  slot is an emitter stall; the catch-up handles it and no reseed is fired. The reseed log now
  carries the emit lag, and the relay logs its offset and PLL slew every second.
- **paced relay (opt-in)**: `--relay-buffer <ms>` holds received packets in a jitter buffer and
  emits them on PHC slots, with a bounded fill controller and a hard latency ceiling; no
  fabricated audio on underrun. The 1Zwolle unit now runs with `--relay-buffer 4 -v`.

Validation before deployment (scratch group, real source in parallel):

- stall test on the exact artefact: 10/30/50 ms sender stalls → 0 reseeds, 150 ms → 1 bounded
  resync + 1 reseed, paced relay steady (fill 3–4 ms, 0 drops/underruns, output 0.75–1.27 ms),
  relay survives a source gap and its own stall without reseeding — `RESULT: PASS`;
- A/B on the live Stereo Tool source (parallel relay to a scratch group): arrival-paced 1000 pps,
  interval min 0.005 / sd 0.090 / max 4.892 ms, 2 intervals >2 ms per 30 s → paced 1000 pps,
  min 0.715 / sd 0.045 / max 1.260 ms, 0 intervals >2 ms; fill steady 4/4 ms, trim 0 ppm,
  slew ±2 ppm, 0 drops (the source is effectively locked to the PHC, so almost no drift).

After deployment (16:03): relay `fill` 5/4 ms, offset ±0.01 ms, `trim` 0, `slew` ±2 ppm,
2 underruns (source hiccups absorbed), 0 drops; relay output 1000 pps, intervals 0.715–1.260 ms,
0 >2 ms; Salland1 1000.1 pps, 0.851–1.152 ms, 0 >2 ms, `fill` 1002.8 ms, 0 underruns; both
processes verified as 1.7.2 via `sha256sum /proc/<pid>/exe`; no reseeds; promiscuous-mode kernel
events 0; `portState SLAVE`, `offsetFromMaster` −464 ns. Rollback: `/usr/local/bin/aes67-tx.1.7.1.bak`
and `/etc/systemd/system/aes67-1zwolle-hd.service.1.7.1.bak`.

The paced relay adds 4 ms of program latency on the 1Zwolle chain; `-L`/`--relay-buffer` accepts
2 ms for minimum latency or 8–10 ms on a jittery network.

Measurement after ~3 h 48 min on 2026-10-24:

| Flow | Average latency | Peak | Late packets | Notes |
|---|---:|---:|---:|---|
| Salland1 HD | **0.245 ms** | 4.8 ms | 17,727 | ~0.13% of packets; PTP-paced |
| 1Zwolle HD | **0.724 ms** | 15 ms | 2,239 | ~0.016%; unbuffered relay |
| DVS reference | 2.2 / 1.7 ms | 2.3 / 2.9 ms | 0 | Windows Dante Virtual Soundcard, 5 ms latency |

Dante gives AES67 flows a fixed 2 ms latency budget; DVS can use its 5 ms setting. The remaining Late packets are short spikes above the 2 ms budget, not a structural offset.

## What works

- PTP hardware-clock slaving with `ptp4l`; Chrony keeps the **system/wall clock** independently. No `phc2sys` or `timemaster` is used.
- Media-clock **PLL/slew**: small RTP↔PTP offsets are corrected over ~30 s at a bounded rate, so timestamps stay continuous. Since v1.7.1 an emitter stall ≤100 ms is caught up with continuous timestamps; only a longer stall or an offset that survives the catch-up is resynchronised (bounded jump) or hard-reseeded.
- Adaptive resampling for `--raw`: a 32-tap Kaiser-windowed sinc with a leaky, clamped PI controller holds the input ring at the target latency. This compensates the remote FLAC encoder clock against the PTP clock and prevents growing latency or zero-packet hiccups.
- ASRC anti-windup: the buffer stays at ~1 s instead of oscillating between ~864 and ~1086 ms.
- Underrun exit (`-U 10`) plus systemd restart: a stopped PCM pipe is recovered automatically instead of silently transmitting zero packets.
- Existing Dante Controller subscriptions and SDP sessions are reused; no new SDP import is needed for routine service changes.
- The SDP examples document the required PTP reference (`a=ts-refclk`); the actual grandmaster identity and domain are network-specific and therefore not recorded here.

## Decisions taken

1. **Reuse the existing multicast groups, SSRC, payload type and SDP sessions.** Service changes must not require a new Dante Controller import.
2. **Liquidsoap is the Salland1 front-end**, not ffmpeg. It provides robust HTTP reconnection/decoding; `mksafe` keeps the process alive during short input loss.
3. **Target 1 s buffer, 3 s ring capacity** for the internet-fed Salland1 flow. This is intentional latency for network robustness; it is not a Dante latency setting.
4. **PLL/slew instead of frequent hard reseeds.** Small offsets are corrected continuously; hard reseed is reserved for a real large step or a stall longer than the bounded catch-up (100 ms).
5. **Chrony and PTP are separate clock domains.** Chrony disciplines the wall clock; `aes67-tx` uses the PTP hardware clock directly. Do not tie the system clock to the Dante clock.
6. **Keep the 1Zwolle webstream chain isolated.** Restarting or changing it is not part of AES67 maintenance.

## Open items

1. **Host-side stalls** were the real cause of the reseed events — not grandmaster steps (see the verification section). v1.7.1 removed the false "PTP step" reseed for stalls ≤100 ms, and the week after deployment showed 1 reseed instead of ~1.5/day. Two things remain open: (a) *what* makes the host stall — candidates are real-time scheduling/preemption (`aes67-tx` runs at nice −10, not SCHED_FIFO; the DAB/dab-scripts and `odr-audioenc` compete for the same cores) and D-state/NFS (load average ~6 while 88% idle); (b) the one remaining reseed of 2026-11-04 04:20:37 (+44.7 ms) had no PTP event either and was seen only by the Salland1 process, so a short backward PHC excursion or a preemption inside the detector window still slips past the catch-up — instrument this in v1.7.2 (log the emit lag at reseed time; catch up instead of reseeding when the loop is behind schedule).
2. **Salland1 input stalls.** Two 10 s underrun restarts on 2026-10-27, again on 2026-11-03 09:19 and 2026-11-04 22:19. `-U 10` plus systemd handled every one correctly and Liquidsoap reconnected, but the upstream cause (HTTP/FLAC delivery) was not identified.
3. **Audio-level watchdog not implemented.** The packet watchdog and underrun timeout cover a dead pipe, but not the case where the decoder keeps sending *silent* PCM with valid packet timing.
4. **Residual Late spikes / relay clock — addressed in v1.7.2.** 1Zwolle's relay now runs paced (`--relay-buffer 4`): the media clock is anchored on the PHC and the jitter buffer smooths the arrival pattern. Measured before/after in 30 s windows: max interval 4.892 → 1.260 ms, sd 0.090 → 0.045 ms, 2 → 0 intervals above the 2 ms Dante budget. Still to decide: whether 4 ms of extra program latency is the right trade (2 ms is possible) and whether the relay should also get an audio-level watchdog.
5. **1Zwolle AES67 relay** runs the same 1.7.1 binary; its webstream Liquidsoap service remains separate and unchanged.

## Next step

Start the next workspace by re-reading this file and the README, then verify the current
service/version and PTP offsets before changing anything. The first priorities are:

- week check done on 2026-11-05: reseed bursts stopped (1 in 7.6 days, relay silent), 0 bounded
  resyncs, promiscuous-mode traffic stayed at 0/min; the upstream underrun restarts (3 and 4 Nov)
  recovered by themselves;
- v1.7.2 is deployed: lag-aware reseed (so the next unexplained offset is classified from the
  emit lag in the log) and the paced relay on the PHC. Watch a week of `journalctl` for
  `hard reseed`, `media clock resynced`, `relay underrun` and `dropped=`;
- identify the host-side stalls (scheduling vs D-state vs NIC), because those remain the only
  source of discontinuities;
- add an audio-level watchdog if silent-PCM failures matter.
