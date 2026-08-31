/*
 * aes67-tx.c
 * ---------------------------------------------------------------------------
 * Publish an existing AES67/RTP audio stream as a PTP-synchronised AES67
 * source on Linux.
 *
 * What it does
 * ------------
 * AES67/Dante receivers are strict: a source is only accepted if its RTP
 * timestamps and RTCP Sender Reports reference the PTP clock of the network's
 * grandmaster.  Standard Linux audio tools (GStreamer, ffmpeg, VLC) send RTP
 * on the *system* clock, which is usually NTP-based and unrelated to the
 * AoIP/PTP domain.  Consequently a Dante/AES67 device will refuse such a
 * stream ("no signal", "no sync", cannot subscribe, ...).
 *
 * This program bridges the gap.  It reads an existing AES67/RTP multicast
 * stream and re-publishes it as a new AES67 multicast source whose sender
 * reports are timestamped against the machine's *PTP hardware clock* (the
 * PHC), which itself is slaved to the grandmaster by ptp4l.  Because the
 * sender reports map the media clock onto the PTP domain clock, an AES67
 * receiver will accept and lock to the stream.
 *
 * Typical use case
 * ----------------
 *   A broadcast processor (e.g. Thimeo Stereo Tool) emits AES67 (LiveWire)
 *   audio on one multicast group.  You want that same audio available on a
 *   mixing console that only accepts Dante/AES67 on another group/domain.
 *   Run this program on a Linux host that is a PTP slave of the console's
 *   grandmaster, and point it at the source and a fresh output group.
 *
 * Requirements
 * ------------
 *   - Linux, with a NIC that exposes a PTP hardware clock (/dev/ptpN) and is
 *     configured as a *PTP slave* (ptp4l in slave-only mode) of the AES67
 *     grandmaster.  See README.md and ptp4l.*.conf in this repository.
 *   - gcc (build) and root privileges at runtime (to open /dev/ptpN and to
 *     join multicast groups).
 *
 * This program deliberately does NOT decode/encode audio: it copies the RTP
 * payload bytes unchanged and only rewrites the RTP header + sender reports.
 * The output format is therefore identical to the input (e.g. L24/48000/2);
 * choose the matching SDP when subscribing the sink.
 *
 * License: MIT — Copyright (c) 2026 Hans van Eijsden / Hans van Eijsden
 * Consultancy (see LICENSE)
 * ---------------------------------------------------------------------------
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>

#define PROGRAM   "aes67-tx"
#define VERSION   "1.4.1"

/* POSIX clock number of a PTP hardware clock (PHC), as used by linuxptp.
 * It is the same magic value for every /dev/ptpN; the kernel resolves it to
 * the clock associated with the currently-open /dev/ptpN. */
#define PHC_CLOCKID ((clockid_t)0xffffffe3L)

/* seconds between the Unix epoch (1970) and the NTP epoch (1900) */
#define NTP_1900   2208988800ULL

/* default PTP clock device */
#define DEFAULT_PTP "/dev/ptp0"

typedef struct {
    char   in_addr[64];      /* input multicast group            */
    int    in_port;          /* input RTP port                   */
    char   out_addr[64];     /* output multicast group           */
    int    out_port;         /* output RTP port                  */
    char   ptp_dev[128];     /* /dev/ptpN                        */
    uint32_t ssrc;           /* output SSRC (0 -> random)        */
    int    pt;               /* output RTP payload type          */
    int    ttl;              /* output multicast TTL             */
    char   iface[64];        /* output interface name (optional) */
    int    sr_interval;      /* RTCP Sender Report interval (s)  */
    int    restamp;          /* 1 = re-stamp ts from PTP clock   */
    int    rate;             /* RTP clock rate for --re-stamp    */
    int    raw;              /* 1 = read raw PCM from stdin      */
    int    bitdepth;         /* PCM bits per sample (16/24)      */
    int    channels;         /* number of channels               */
    int    pkt;              /* samples (frames) per RTP packet  */
    int    jbuf;             /* jitter-buffer depth, in packets  */
    int    verbose;          /* log progress                     */
} cfg_t;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "%s %s - publish an AES67/RTP stream as a PTP-synchronised AES67 source\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Input source (one of)\n"
        "  -A, --input-addr <addr>   input multicast group\n"
        "  -P, --input-port <port>   input RTP port        (default 5004)\n"
        "  -S, --sdp <file>          read input address/port from an SDP file\n"
        "\n"
        "Output AES67 source\n"
        "  -a, --output-addr <addr>  output multicast group\n"
        "  -p, --output-port <port>  output RTP port       (default 5004)\n"
        "  -k, --ssrc <hex>          output SSRC           (0 = random)\n"
        "  -t, --pt <n>              output payload type   (default 96)\n"
        "  -l, --ttl <n>             multicast TTL         (default 8)\n"
        "  -f, --iface <name>        output interface, e.g. eth0\n"
        "\n"
        "PTP / timing\n"
        "  -d, --ptp-device <path>   PTP hardware clock    (default " DEFAULT_PTP ")\n"
        "  -r, --sr-interval <sec>   Sender Report interval (default 1)\n"
        "  -R, --re-stamp            re-stamp RTP timestamps from the PTP clock (default)\n"
        "  -K, --keep-ts             forward the source RTP timestamps instead\n"
        "                            (only if the source already follows the grandmaster;\n"
        "                            recommended: leave -R on for non-PTP sources)\n"
        "  -B, --rate <hz>           RTP clock rate for --re-stamp (default 48000)\n"
        "  -W, --raw                 read raw PCM on stdin instead of an RTP source\n"
        "  -b, --bit <16|24>         PCM bits per sample for --raw    (default 24)\n"
        "  -C, --channels <n>        channels for --raw               (default 2)\n"
        "  -n, --pkt <frames>        frames per RTP packet for --raw  (default 48 = 1 ms)\n"
        "  -J, --jitter <pkts>       jitter-buffer depth for --raw    (default 32;\n"
        "                            read-ahead this many packets and emit one per media\n"
        "                            slot, smoothing bursts/gaps from the upstream)\n"
        "\n"
        "Other\n"
        "  -v, --verbose             log progress to stderr\n"
        "  -h, --help                this help\n"
        "\n"
        "Example:\n"
        "  sudo %s -A 239.192.19.137 -P 5004 -a 239.69.100.1 -p 5004 -f eth0 -v\n",
        PROGRAM, VERSION, prog, PROGRAM);
}

/* minimal SDP parser: return multicast address + RTP port */
static int parse_sdp(const char *path, char *addr, int *port)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror("sdp"); return -1; }
    char line[512]; int found = 0;
    *port = 0; addr[0] = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "c=IN IP4 ", 9) == 0) {
            if (sscanf(line, "c=IN IP4 %63[^/ \r\n]", addr) == 1) { found = 1; }
        } else if (strncmp(line, "m=audio ", 8) == 0) {
            sscanf(line, "m=audio %d", port);
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* read a PTP hardware clock.  The caller must have opened the PHC device
 * (so the kernel associates the magic clockid with it). */
static double phc_sec(void)
{
    struct timespec ts;
    if (clock_gettime(PHC_CLOCKID, &ts) != 0) return -1;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* resolve an interface name to its IPv4 address (0.0.0.0 if not found). */
static uint32_t iface_ipv4(const char *name)
{
    struct ifaddrs *ifa, *p;
    uint32_t addr = htonl(INADDR_ANY);
    if (getifaddrs(&ifa) != 0) return addr;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, name) == 0) {
            addr = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
            break;
        }
    }
    freeifaddrs(ifa);
    return addr;
}

/* Send an RTCP Sender Report, mapping the current media clock (lastts) to the
 * PTP domain time.  Sent to the RTCP port (RTP port + 1) on the output group. */
static void send_sr(int tx, struct sockaddr_in *txa, uint32_t ssrc,
                    uint64_t pktcnt, uint64_t octet, uint32_t lastts)
{
    double t = phc_sec();
    if (t < 0) return;
    uint8_t sr[28]; memset(sr, 0, 28);
    sr[0] = 0x80; sr[1] = 200; sr[2] = 0; sr[3] = 6;      /* SR */
    sr[4] = (ssrc>>24)&0xff; sr[5] = (ssrc>>16)&0xff; sr[6] = (ssrc>>8)&0xff; sr[7] = ssrc&0xff;
    double nt = t + (double)NTP_1900;
    uint32_t ns = (uint32_t)nt, nf = (uint32_t)((nt - ns) * 4294967296.0);
    sr[8] = (ns>>24)&0xff;  sr[9] = (ns>>16)&0xff;  sr[10] = (ns>>8)&0xff;  sr[11] = ns&0xff;
    sr[12] = (nf>>24)&0xff; sr[13] = (nf>>16)&0xff; sr[14] = (nf>>8)&0xff;  sr[15] = nf&0xff;
    sr[16] = (lastts>>24)&0xff; sr[17] = (lastts>>16)&0xff; sr[18] = (lastts>>8)&0xff; sr[19] = lastts&0xff;
    sr[20] = (pktcnt>>24)&0xff; sr[21] = (pktcnt>>16)&0xff; sr[22] = (pktcnt>>8)&0xff; sr[23] = pktcnt&0xff;
    sr[24] = (octet>>24)&0xff;  sr[25] = (octet>>16)&0xff;  sr[26] = (octet>>8)&0xff;  sr[27] = octet&0xff;
    struct sockaddr_in ra = *txa; ra.sin_port = htons(ntohs(txa->sin_port) + 1);
    sendto(tx, sr, 28, 0, (struct sockaddr *)&ra, sizeof(ra));
}

int main(int argc, char **argv)
{
    cfg_t c;
    memset(&c, 0, sizeof c);
    c.in_port = 5004; c.out_port = 5004; c.pt = 96; c.ttl = 8;
    c.sr_interval = 1; c.ssrc = 0; c.rate = 48000; c.iface[0] = 0;
    c.bitdepth = 24; c.channels = 2; c.pkt = 48; c.raw = 0;
    c.jbuf = 32;   /* jitter-buffer depth (packets) for --raw; ~32 ms at 48k/48 */
    /* Default: re-stamp from the PTP clock. A non-PTP source (e.g. Stereo Tool)
     * uses the system clock, so forwarding its timestamps is not in lockstep
     * with the grandmaster and Dante keeps the channel muted. Re-stamping from
     * the PHC makes the media clock run in lockstep with the domain. */
    c.restamp = 1;
    strcpy(c.ptp_dev, DEFAULT_PTP);
    snprintf(c.in_addr, 64, "239.192.19.137");
    snprintf(c.out_addr, 64, "239.69.100.1");

    static const struct option opts[] = {
        {"input-addr",   required_argument, 0, 'A'},
        {"input-port",   required_argument, 0, 'P'},
        {"sdp",          required_argument, 0, 'S'},
        {"output-addr",  required_argument, 0, 'a'},
        {"output-port",  required_argument, 0, 'p'},
        {"ssrc",         required_argument, 0, 'k'},
        {"pt",           required_argument, 0, 't'},
        {"ttl",          required_argument, 0, 'l'},
        {"iface",        required_argument, 0, 'f'},
        {"ptp-device",   required_argument, 0, 'd'},
        {"sr-interval",  required_argument, 0, 'r'},
        {"re-stamp",     no_argument,       0, 'R'},
        {"keep-ts",      no_argument,       0, 'K'},
        {"rate",         required_argument, 0, 'B'},
        {"raw",          no_argument,       0, 'W'},
        {"bit",          required_argument, 0, 'b'},
        {"channels",     required_argument, 0, 'C'},
        {"pkt",          required_argument, 0, 'n'},
        {"jitter",       required_argument, 0, 'J'},
        {"verbose",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int copt;
    while ((copt = getopt_long(argc, argv, "A:P:S:a:p:k:t:l:f:d:r:RB:KWb:C:n:J:vh", opts, NULL)) != -1) {
        switch (copt) {
        case 'A': snprintf(c.in_addr, 64, "%s", optarg); break;
        case 'P': c.in_port = atoi(optarg); break;
        case 'S': if (parse_sdp(optarg, c.in_addr, &c.in_port) != 0) {
                      fprintf(stderr, "cannot parse SDP %s\n", optarg); return 1;
                  } break;
        case 'a': snprintf(c.out_addr, 64, "%s", optarg); break;
        case 'p': c.out_port = atoi(optarg); break;
        case 'k': c.ssrc = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 't': c.pt = atoi(optarg); break;
        case 'l': c.ttl = atoi(optarg); break;
        case 'f': snprintf(c.iface, 64, "%s", optarg); break;
        case 'd': snprintf(c.ptp_dev, 128, "%s", optarg); break;
        case 'r': c.sr_interval = atoi(optarg); break;
        case 'R': c.restamp = 1; break;
        case 'K': c.restamp = 0; break;
        case 'B': c.rate = atoi(optarg); break;
        case 'W': c.raw = 1; break;
        case 'b': c.bitdepth = atoi(optarg); break;
        case 'C': c.channels = atoi(optarg); break;
        case 'n': c.pkt = atoi(optarg); break;
        case 'J': c.jbuf = atoi(optarg); break;
        case 'v': c.verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default : usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* --- PTP hardware clock ------------------------------------------ */
    int phc = open(c.ptp_dev, O_RDWR);
    if (phc < 0) { perror("open ptp device"); return 1; }
    if (phc_sec() < 0) {
        fprintf(stderr, "cannot read PTP clock from %s (is the NIC a PTP slave?)\n", c.ptp_dev);
        return 1;
    }

    /* Measure the PTP clock rate (how many PTP seconds pass per MONOTONIC second).
     * The grandmaster is often a *free-running* class-248 clock (e.g. a Dante bridge)
     * whose oscillator is not exactly 48000/48000; the PTP slave tracks it.  A media
     * clock advanced by the plain sample count runs at the *system* (MONOTONIC) rate,
     * which drifts a few ppm from the free-running grandmaster -> over a long session
     * the receiver's jitter buffer overruns ("Late", latency climbing).  Advancing the
     * media clock by 'sample_step * phc_rate' keeps it in lockstep with the grandmaster. */
    double phc_rate = 1.0;
    {
        struct timespec tt; double m0, m1, p0, p1;
        usleep(1000000);                              /* let the slave settle a moment */
        clock_gettime(CLOCK_MONOTONIC, &tt); m0 = tt.tv_sec + tt.tv_nsec / 1e9;
        p0 = phc_sec(); if (p0 < 0) p0 = 0;
        usleep(2000000);
        clock_gettime(CLOCK_MONOTONIC, &tt); m1 = tt.tv_sec + tt.tv_nsec / 1e9;
        p1 = phc_sec(); if (p1 < 0) p1 = p0 + (m1 - m0);
        if (m1 > m0) phc_rate = (p1 - p0) / (m1 - m0);
    }
    if (c.verbose) fprintf(stderr, "  ptp     phc_rate=%.6f (media clock locked to PTP rate)\n", phc_rate);

    /* --- input socket (RTP source): join the multicast group -------- */
    int rx = -1;
    if (!c.raw) {
        rx = socket(AF_INET, SOCK_DGRAM, 0);
        if (rx < 0) { perror("rx socket"); return 1; }
        int one = 1;
        setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(rx, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        struct sockaddr_in rxa; memset(&rxa, 0, sizeof rxa);
        rxa.sin_family = AF_INET;
        rxa.sin_addr.s_addr = inet_addr(c.in_addr);
        rxa.sin_port = htons(c.in_port);
        struct ip_mreq m; memset(&m, 0, sizeof m);
        m.imr_multiaddr.s_addr = inet_addr(c.in_addr);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0) {
            perror("IP_ADD_MEMBERSHIP"); return 1;
        }
        if (bind(rx, (struct sockaddr *)&rxa, sizeof rxa) < 0) {
            perror("bind rx"); return 1;
        }
    }

    /* --- output socket ---------------------------------------------- */
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0) { perror("tx socket"); return 1; }
    unsigned char ttl = (unsigned char)(c.ttl & 0xff);
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    unsigned char loop = 1;   /* allow local monitoring of our own output */
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    struct in_addr ifa;
    if (c.iface[0]) ifa.s_addr = iface_ipv4(c.iface);
    else            ifa.s_addr = htonl(INADDR_ANY);
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
    struct sockaddr_in txa; memset(&txa, 0, sizeof txa);
    txa.sin_family = AF_INET;
    txa.sin_addr.s_addr = inet_addr(c.out_addr);
    txa.sin_port = htons(c.out_port);

    uint32_t ssrc = c.ssrc ? c.ssrc : (uint32_t)((getpid() ^ (uint32_t)time(NULL)) & 0x7fffffff);
    uint32_t seq = (uint32_t)time(NULL) & 0xff;
    uint64_t pktcnt = 0, octet = 0; uint32_t lastts = 0; time_t lastSR = 0, lastLog = 0;
    /* Smooth, PTP-seeded media clock.  Re-stamping every packet from the raw PHC
     * read (phc_sec()*rate) jitters (a hardware-clock read + the servo are not
     * perfectly even), which shows up as a ragged/overdriven distortion on a strict
     * receiver.  Seeding once from the PHC and advancing by the exact sample count
     * per packet gives a clean, even media clock that is still PTP-referenced via
     * the Sender Reports. */
    uint32_t media_ts = 0;
    double   mts_acc  = 0.0;   /* fractional media-clock accumulator (raw mode, PTP-rate locked) */

    if (c.verbose) {
        fprintf(stderr, "%s %s\n", PROGRAM, VERSION);
        if (c.raw)
            fprintf(stderr, "  input   stdin (raw %s%d, %d Hz, %d ch, %d frames/pkt)\n",
                    c.bitdepth == 24 ? "S24LE " : "S16LE ", c.bitdepth, c.rate, c.channels, c.pkt);
        else
            fprintf(stderr, "  input   %s:%d\n", c.in_addr, c.in_port);
        fprintf(stderr, "  output  %s:%d  (ttl=%d iface=%s pt=%d ssrc=0x%08x)\n",
                c.out_addr, c.out_port, c.ttl, c.iface[0] ? c.iface : "(auto)", c.pt, ssrc);
        fprintf(stderr, "  ptp     %s  restamp=%s  sr_every=%ds\n",
                c.ptp_dev, c.restamp ? "yes" : "no (forward source ts)", c.sr_interval);
    }

    unsigned char in[2048], out[1600];

    if (c.raw) {
        /* ---- raw PCM input from stdin -------------------------------- */
        int bytes_per_frame = (c.bitdepth / 8) * c.channels;
        int packet_bytes    = c.pkt * bytes_per_frame;

        /* Jitter buffer: read ahead into a small ring of packets, then emit one
         * packet per media-clock slot.  A bursty upstream (e.g. ffmpeg decoding a
         * network FLAC stream) delivers data in bursts and gaps; instead of letting
         * the raw stdin arrival times drive the network (which makes an AES67
         * receiver's jitter buffer overflow -> "Late" spikes / link drops), we drain
         * stdin into the ring and emit at an even, rock-solid pace. */
        int jb = c.jbuf > 0 ? c.jbuf : 32;
        unsigned char **jbq = calloc((size_t)jb, sizeof *jbq);
        for (int i = 0; i < jb; i++) jbq[i] = malloc((size_t)packet_bytes);
        unsigned char *cur = malloc((size_t)packet_bytes);
        int got = 0, jh = 0, jl = 0;               /* ring head + fill level */

        /* Media clock: stamp each emitted packet from the PTP clock.  This keeps the
         * source PTP-referenced, which is what a strict AES67/Dante receiver requires
         * (a clock locked to the content rate is rejected).  If the upstream content
         * runs at a slightly different rate, the receiving device resamples it to the
         * session rate; the jitter buffer below smooths the packet timing independently. */
        double pkt_period = (double)c.pkt / (double)c.rate;   /* e.g. 48/48000 = 1 ms */
        if (c.verbose)
            fprintf(stderr, "  raw: %d bytes (%d frames)/pkt, paced %.2f ms, jitter buffer %d pkts (~%.0f ms)\n",
                    packet_bytes, c.pkt, pkt_period * 1000.0, jb, pkt_period * jb * 1000.0);

        /* make stdin non-blocking so we can poll it against the send schedule */
        int f_orig = fcntl(0, F_GETFL, 0);
        fcntl(0, F_SETFL, f_orig | O_NONBLOCK);

        /* pre-fill the jitter buffer so it can immediately absorb a burst */
        int eof = 0;
        while (jl < jb && !eof) {
            int r = (int)read(0, cur + got, packet_bytes - got);
            if (r < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) break; eof = 1; break; }
            if (r == 0) { eof = 1; break; }
            got += r;
            if (got == packet_bytes) { memcpy(jbq[(jh + jl) % jb], cur, packet_bytes); jl++; got = 0; }
        }

        struct timespec nowt; clock_gettime(CLOCK_MONOTONIC, &nowt);
        double base_s = nowt.tv_sec + nowt.tv_nsec / 1e9;
        uint64_t slot = 0;

        while (!g_stop) {
            /* 1) drain any ready stdin bytes into the ring */
            while (jl < jb && !eof) {
                int r = (int)read(0, cur + got, packet_bytes - got);
                if (r < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) break; eof = 1; break; }
                if (r == 0) { eof = 1; break; }
                got += r;
                if (got == packet_bytes) { memcpy(jbq[(jh + jl) % jb], cur, packet_bytes); jl++; got = 0; }
            }
            /* 2) emit one packet when its media-clock slot is due */
            clock_gettime(CLOCK_MONOTONIC, &nowt);
            double now_s = nowt.tv_sec + nowt.tv_nsec / 1e9;
            double target = base_s + (double)slot * pkt_period;
            if (jl > 0 && now_s >= target) {
                /* if an upstream gap made the schedule fall behind, restart it so we
                 * never emit a burst of packets to "catch up" */
                if (now_s - target > pkt_period) { base_s = now_s; slot = 0; }
                unsigned char *p = jbq[jh];
                if (mts_acc == 0) { double s0 = phc_sec(); if (s0 < 0) s0 = 0; mts_acc = s0 * (double)c.rate; }
                uint32_t ts = (uint32_t)mts_acc;
                mts_acc += (double)c.pkt * phc_rate;        /* advance at the PTP rate (no drift) */
                out[0]=0x80; out[1]=(unsigned char)(c.pt & 0x7f);
                out[2]=(seq>>8)&0xff; out[3]=seq&0xff;
                out[4]=(ts>>24)&0xff; out[5]=(ts>>16)&0xff; out[6]=(ts>>8)&0xff; out[7]=ts&0xff;
                out[8]=(ssrc>>24)&0xff; out[9]=(ssrc>>16)&0xff; out[10]=(ssrc>>8)&0xff; out[11]=ssrc&0xff;
                /* AES67 L16/L24 are big-endian; input PCM (from ffmpeg) is little-endian. */
                {
                    int spb = c.bitdepth / 8;               /* 2 (16-bit) or 3 (24-bit) */
                    int samples = c.pkt * c.channels;
                    uint8_t *dst = out + 12;
                    if (spb == 3) {
                        for (int s = 0; s < samples; s++) { int o = s * 3;
                            dst[o] = p[o+2]; dst[o+1] = p[o+1]; dst[o+2] = p[o]; }
                    } else {
                        for (int s = 0; s < samples; s++) { int o = s * 2;
                            dst[o] = p[o+1]; dst[o+1] = p[o]; }
                    }
                }
                sendto(tx, out, 12 + packet_bytes, 0, (struct sockaddr *)&txa, sizeof txa);
                seq++; lastts = ts; pktcnt++; octet += packet_bytes;
                jh = (jh + 1) % jb; jl--; slot++;

                time_t now = time(NULL);
                if (c.sr_interval > 0 && now != lastSR) { lastSR = now; send_sr(tx,&txa,ssrc,pktcnt,octet,lastts); }
                if (c.verbose && now != lastLog) { lastLog = now;
                    fprintf(stderr, "seq=%u ts=%u pkt=%llu\n", seq, ts, (unsigned long long)pktcnt); }
            } else if (jl == 0 && eof) {
                if (c.verbose) fprintf(stderr, "input EOF (upstream stopped) - exiting so systemd restarts\n");
                break;
            } else {
                /* Not emitting right now.
                 *  - ring has data: sleep *precisely* to the next media-clock slot with
                 *    clock_nanosleep (absolute).  Poll's timeout is millisecond-granular so
                 *    a sub-ms remaining wait truncates to 0 -> busy-spin; this avoids that
                 *    and keeps the emit on time without burning a core.
                 *  - ring is empty (waiting on upstream data): poll stdin for a short
                 *    while; never use a 0ms timeout here or we spin during an upstream
                 *    stall. */
                if (jl > 0) {
                    struct timespec ts;
                    ts.tv_sec = (time_t)target;
                    ts.tv_nsec = (long)((target - (double)ts.tv_sec) * 1e9);
                    if (ts.tv_nsec > 999999999L) ts.tv_nsec = 999999999L;
                    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
                } else {
                    struct pollfd p; p.fd = 0; p.events = POLLIN;
                    poll(&p, 1, 4);
                }
            }
        }
        for (int i = 0; i < jb; i++) free(jbq[i]);
        free(jbq); free(cur);
    } else {
        /* ---- RTP source input --------------------------------------- */
        struct pollfd pf; pf.fd = rx; pf.events = POLLIN;
        while (!g_stop) {
            int pr = poll(&pf, 1, 1000);
            if (pr <= 0) continue;
            int n = recvfrom(rx, in, sizeof in, 0, NULL, NULL);
            if (n < 12) continue;
            if ((in[0] >> 6) != 2) continue;                    /* RTP v2 only */
            /* RTP header size: 12 + CSRC count + optional extension */
            int hdr = 12 + 4 * (in[0] & 0x0f);
            if (in[0] & 0x10) {
                if (n < hdr + 4) continue;
                unsigned len = ((unsigned)in[hdr+2] << 8) | in[hdr+3];
                hdr += 4 + 4 * (int)len;
            }
            if (hdr >= n) continue;
            int plen = n - hdr;
            if (plen <= 0) continue;

            uint32_t ts;
            if (c.restamp) {
                /* Smooth media clock: seed once from the PHC, then advance by the exact
                 * sample count per packet.  Re-reading the raw PHC each packet jitters. */
                int bps = c.bitdepth / 8, nch = c.channels > 0 ? c.channels : 2;
                int samples_per_pkt = plen / (bps * nch);
                if (samples_per_pkt <= 0) samples_per_pkt = c.pkt;
                if (media_ts == 0) { double s0 = phc_sec(); if (s0 < 0) s0 = 0; media_ts = (uint32_t)(s0 * (double)c.rate); }
                ts = media_ts;
                media_ts += (uint32_t)samples_per_pkt;
            } else {
                ts = ((uint32_t)in[4]<<24)|((uint32_t)in[5]<<16)|((uint32_t)in[6]<<8)|(uint32_t)in[7];
            }

            out[0]=0x80; out[1]=(unsigned char)(c.pt & 0x7f);
            out[2]=(seq>>8)&0xff; out[3]=seq&0xff;
            out[4]=(ts>>24)&0xff; out[5]=(ts>>16)&0xff; out[6]=(ts>>8)&0xff; out[7]=ts&0xff;
            out[8]=(ssrc>>24)&0xff; out[9]=(ssrc>>16)&0xff; out[10]=(ssrc>>8)&0xff; out[11]=ssrc&0xff;
            memcpy(out+12, in+hdr, plen);
            if (sendto(tx, out, 12+plen, 0, (struct sockaddr *)&txa, sizeof txa) < 0) {
                if (c.verbose && errno != EAGAIN) perror("sendto");
            }
            seq++; lastts=ts; pktcnt++; octet += plen;

            time_t now = time(NULL);
            if (c.sr_interval > 0 && now != lastSR) { lastSR = now; send_sr(tx,&txa,ssrc,pktcnt,octet,lastts); }
            if (c.verbose && now != lastLog) {
                lastLog = now;
                fprintf(stderr, "seq=%u ts=%u pkt=%llu\n", seq, ts, (unsigned long long)pktcnt);
            }
        }
    }

    fprintf(stderr, "stopped: %llu packets, %llu audio bytes sent\n",
            (unsigned long long)pktcnt, (unsigned long long)octet);
    close(rx); close(tx); close(phc);
    return 0;
}
