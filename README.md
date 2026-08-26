# aes67-tx

Publish an existing AES67/RTP audio stream as a **PTP-synchronised AES67 (Dante-compatible) source** on Linux.

Many broadcast AES67/Dante receivers (mixers, MADI bridges, Dante Controllers) are strict: a source is only accepted if its RTP timestamps and RTCP **Sender Reports** reference the **PTP clock of the network's grandmaster**. Standard Linux audio tools (GStreamer, ffmpeg, VLC) send RTP on the *system* clock — usually NTP-based and unrelated to the audio-over-IP/PTP domain — so a Dante/AES67 device refuses the stream ("no signal", "no sync", "cannot subscribe: RTP not enabled", …).

`aes67-tx` bridges that gap. It reads an existing AES67/RTP multicast stream and re-publishes it as a new AES67 multicast source whose Sender Reports are timestamped against the machine's **PTP hardware clock (PHC)**, which `ptp4l` slaves to the grandmaster. Because the Sender Reports map the media clock onto the PTP domain clock, an AES67 receiver accepts and locks to the stream.

It is a small, single-file C program with no runtime dependencies beyond a PTP-capable NIC.

---

## Typical use case

A broadcast processor (e.g. **Thimeo Stereo Tool**) emits AES67 / LiveWire audio on one multicast group. You want that same audio available on a mixing console (or a Dante→MADI bridge) that only accepts Dante/AES67 on another group / clock domain.

```
Stereo Tool  ── AES67/RTP ──>  [ NIC eth0 ] aes67-tx  ── AES67/RTP (PTP) ──>  Dante/AES67 console
                                                                                 (via Dante Controller)
```

The host running `aes67-tx` must be a **PTP slave of the console's grandmaster**.

---

## How it works

- It forwards the RTP **payload bytes unchanged** (no decode/encode). The output format is therefore the same as the input (e.g. `L24/48000/2`).
- It rewrites the RTP header: new SSRC, new sequence number, and — by default — keeps the source's RTP timestamps.
- It sends **RTCP Sender Reports** every second. The Sender Report's timestamp is taken from the **PHC** (the NIC's PTP-synchronised hardware clock), so the receiver can lock the media clock to the PTP grandmaster.
- `--re-stamp` instead timestamps each packet directly from the PHC (see below).

---

## Requirements

- Linux with a NIC that exposes a **PTP hardware clock** (`/dev/ptpN`, e.g. an Intel I219/V2V, or many server NICs). Verify: `ethtool -T eth0` shows `hardware-receive`/`hardware-raw-clock` and a "Hardware timestamp provider".
- A **PTP grandmaster** on the network (a Dante/AES67 device often fulfils this role).
- The host configured as a **PTP slave** of that grandmaster with `ptp4l` (`/dev/ptp0` then reflects the domain time). See [Setting up the PTP slave](#setting-up-the-ptp-slave).
- **root** privileges at runtime (to open `/dev/ptpN` and join multicast groups).
- `gcc`/`cc` to build.

The host does **not** need to be NTP-synchronised; run `ptp4l` slave and `chrony`/NTP independently (recommended: keep the system clock on NTP and only use the PHC for AES67 timing).

---

## Build

```sh
make
# or
cc -O2 -Wall -Wextra -o aes67-tx aes67-tx.c

sudo make install   # optional: installs to /usr/local/bin/aes67-tx
```

---

## Quick start

1. Make the host a PTP slave (see below), so `/dev/ptp0` follows the grandmaster.
2. Run the transmitter, reading your source and writing to a fresh multicast group:

```sh
sudo ./aes67-tx \
  -A 239.192.19.137 -P 5004 \      # the existing AES67/RTP source
  -a 239.69.100.1   -p 5004 \      # the new AES67 output group
  -f eth0 -v
```

3. In **Dante Controller** (or your AES67 controller): **File → External SDP Sessions → Add**, paste the matching SDP (see `example-sdp.txt`), then subscribe the new source to your device (bridge/mixer inputs) and route it.

The new stream should now be accepted with a green/locked flow and produce audio.

---

## Options

```
-a, --output-addr <addr>   output multicast group            (required)
-p, --output-port <port>   output RTP port                   (default 5004)
-A, --input-addr <addr>    input multicast group             (one of the two)
-P, --input-port <port>    input RTP port                    (default 5004)
-S, --sdp <file>           read input addr/port from an SDP file
-k, --ssrc <hex>           output SSRC                       (0 = random)
-t, --pt <n>               output RTP payload type           (default 96)
-l, --ttl <n>              multicast TTL                     (default 8)
-f, --iface <name>         output interface, e.g. eth0
-d, --ptp-device <path>    PTP hardware clock                (default /dev/ptp0)
-r, --sr-interval <sec>    Sender Report interval            (default 1)
-R, --re-stamp             re-stamp RTP timestamps from the PTP clock
-B, --rate <hz>            RTP clock rate for --re-stamp     (default 48000)
-v, --verbose              log progress to stderr
-h, --help                 show help
```

### Choosing the input

Use either `-A/-P` (address and port) or `-S <sdp-file>`. An SDP is handy when you have the source's session description (Stereo Tool, RAVENNA, etc.); the `c=` and `m=audio` lines are read.

### Choosing the output timestamp strategy

- **Default (forward source timestamps):** the media clock keeps the source's rate and phase; the Sender Reports map it onto the PTP domain time. This is the usual case and works well when the source and the grandmaster are close.
- **`--re-stamp`:** each RTP packet is timestamped directly from the PHC, making the media clock *itself* the PTP domain clock. Use this when you want a strictly PTP-locked source and the source rate closely matches the PHC rate. Specify the clock rate with `--rate`.

### Output interface

Set `-f <iface>` to the interface that carries the output multicast (the one wired to the Dante/AES67 switch). If omitted, the kernel picks the default route interface.

---

## Setting up the PTP slave

The host must expose a PTP-slaved hardware clock. A minimal, **slave-only** `ptp4l` configuration:

```ini
# /etc/linuxptp/aes67.conf
[global]
domainNumber        0
network_transport    UDPv4
delay_mechanism     E2E
```

Run it in **slave-only** mode so this host never becomes a grandmaster (important — you do not want to corrupt the domain):

```sh
sudo ptp4l -i eth0 -s -f /etc/linuxptp/aes67.conf
```

Check it found and follows the grandmaster:

```sh
pmc -u -b 0 "GET PORT_DATA_SET"      # expect portState = SLAVE
pmc -u -b 0 "GET TIME_STATUS_NP"     # expect gmPresent = true and a small master_offset
```

A ready-to-use systemd unit is in **`ptp4l-aes67.service`** (adjust the interface and PTP device). The host's *system clock* can stay on `chrony`/NTP; the PHC is what matters for AES67.

> **Troubleshooting:** if `ptp4l` stays `UNCALIBRATED`, the grandmaster may only answer **unicast** Sender/Delay requests (a common Dante behaviour). Add `hybrid_e2e 1` to the `[global]` section:
> ```ini
> hybrid_e2e          1
> ```
> This makes the slave send its delay request directly to the grandmaster's address rather than as a multicast request.

---

## Example SDP

The output stream is described by an SDP you paste into the controller. It must match the output **payload type**, **media** (`c=`), **port** (`m=audio`), and **encoding**. For a `L24/48000/2` output:

```sdp
v=0
o=- 1 1 IN IP4 <host-ip>
s=AES67-tx re-publication
c=IN IP4 239.69.100.1
t=0 0
m=audio 5004 RTP/AVP 96
a=rtpmap:96 L24/48000/2
a=ptime:1
a=mediaclk:direct=0
```

- Replace `<host-ip>` with the IP of the host running `aes67-tx`.
- Replace the `c=`, `m=audio` and payload type to match your actual output (`-a`, `-p`, `-t`).

Two ready-made examples are in the repository: `example-sdp.txt` (no `ts-refclk`, accepted by most controllers) and `example-sdp-refclk.txt` (adds the PTP reference).

### Subscribing in Dante Controller

1. **File → External SDP Sessions → Add**, paste the SDP, OK. The source should appear.
2. Select your device (the mixer, the MADI bridge, the DVS host). If a receiver complains **"RTP not enabled on RX device"**, enable **AES67** in that device's config — note that *Dante Virtual Soundcard (DVS)* is Dante-only and does **not** receive AES67; use a hardware Dante/AES67 device instead.
3. Subscribe the new channels and route them to the output, then confirm the flow shows signal/audio.

---

## Where the audio ends up

The newly published source is a normal AES67 multicast stream on the output group. You can route it to any AES67/Dante receiver. Because the receiver is typically a hybrid Dante/AES67 network, you normally:

- enable AES67 on the receiving device, and
- subscribe the source from the "External SDP Session" entry, then
- assign that flow to the desired Dante channels / MADI outputs.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Source appears but subscription is refused, or flow is red | The stream is not PTP-synced. Make sure the host is a PTP slave (`portState = SLAVE`) and Sender Reports are being sent (`tcpdump ... port 5005`). |
| "RTP not enabled on RX device" | Enable AES67 on the receiving device. DVS (Dante Virtual Soundcard) has no AES67 receive — use a hardware Dante/AES67 device. |
| `ptp4l` stuck in `UNCALIBRATED` | The grandmaster only answers unicast delay requests — add `hybrid_e2e 1` (see above). |
| Flow is green but no signal on the mixer | The destination (e.g. a MADI/console output) may be inactive, or the AES67 source needs `ts-refclk` to fully lock. Try `example-sdp-refclk.txt`. |
| No audio data / silence | Check the source multicast is flowing (`tcpdump -i eth0 "udp port 5004"`), that you are using the right interface (`-f`), and that the source is not silent. |
| Sender Report interval too aggressive | `--sr-interval` is in seconds; leave at least 1. |

---

## License

MIT License. Copyright © 2026 **Hans van Eijsden / Hans van Eijsden Consultancy**. See `LICENSE`.

## Credits / context

This tool was created out of a real broadcast setup: publishing a Stereo Tool AES67/LiveWire stream onto a Dante network (VertoMX → MADI → console). It is intentionally small and dependency-free so it is easy to audit and adapt.
