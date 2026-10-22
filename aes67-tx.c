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
#include <math.h>
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
#define VERSION   "1.6.1"

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
    int    jbuf;             /* raw input ring capacity, in packets */
    int    latency_ms;       /* target buffer latency for --raw (ms); 0 = auto
                                (half of the -J capacity).  The adaptive resampler
                                holds the input buffer at this fill level. */
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
        "  -J, --jitter <pkts>       input ring capacity for --raw   (default 32;\n"
        "                            total buffering capacity in packets.  Use a large\n"
        "                            value for internet sources, e.g. -J 4096 = ~4 s)\n"
        "  -L, --latency <ms>        target buffer latency for --raw  (default 0 =\n"
        "                            half of the -J capacity).  The adaptive resampler\n"
        "                            locks the input buffer at this fill level, so clock\n"
        "                            drift and network jitter never accumulate into\n"
        "                            growing latency or hiccups.\n"
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

/* Difference (in samples) between the current PTP/PHC time and an RTP media
 * timestamp, wrapped to ±2^31 samples.  The media clock is seeded from the PHC
 * at startup, so in normal operation this is within about a millisecond.  A
 * PTP step (grandmaster reboot/reconfiguration) leaves the media clock at the
 * old offset; the main loops use this value to detect and correct that. */
static long ptp_media_diff(int rate, uint32_t media_ts)
{
    double now = phc_sec();
    if (now < 0) return 0;
    double phc_samples = fmod(now * (double)rate, 4294967296.0);
    double d = phc_samples - (double)media_ts;
    d = fmod(d, 4294967296.0);
    if (d > 2147483648.0) d -= 4294967296.0;
    if (d < -2147483648.0) d += 4294967296.0;
    return (long)llround(d);
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

/* --- adaptive windowed-sinc resampler for --raw -------------------------
 *
 * The input is a free-running PCM stream: its sample rate is set by the
 * *remote* encoder's clock, not by our PTP clock.  Even a perfect 48 kHz
 * FLAC stream therefore drifts a few ppm against the grandmaster.  Feeding
 * that fixed-rate content to a PTP-paced output makes the input ring slowly
 * fill (growing latency) or empty (zero-packet hiccups).
 *
 * We hold the ring at a fixed target fill and continuously adapt the
 * resampling ratio with a slow PI controller.  A 32-tap Kaiser-windowed sinc
 * keeps the audio quality high; the ratio only changes by ppm, so pitch
 * modulation is inaudible.  The output itself stays exactly PTP-paced.
 */
#define RS_TAPS   32
#define RS_PHASES 1024
#define RS_BETA   8.0
static double rs_coef[RS_PHASES][RS_TAPS];

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 30; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

static void rs_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    double i0b = bessel_i0(RS_BETA);
    for (int p = 0; p < RS_PHASES; p++) {
        double frac = (double)p / (double)RS_PHASES;
        double sum = 0.0;
        for (int t = 0; t < RS_TAPS; t++) {
            double x = (double)t - (double)(RS_TAPS / 2 - 1) - frac;
            double s = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            double r = ((double)t - (double)(RS_TAPS - 1) / 2.0) /
                       ((double)(RS_TAPS - 1) / 2.0);
            double w = 0.0;
            if (fabs(r) <= 1.0) w = bessel_i0(RS_BETA * sqrt(1.0 - r * r)) / i0b;
            rs_coef[p][t] = s * w;
            sum += rs_coef[p][t];
        }
        if (fabs(sum) > 1e-12)
            for (int t = 0; t < RS_TAPS; t++) rs_coef[p][t] /= sum;
    }
}

/* Interpolate one output sample from the interleaved source buffer at
 * fractional source position `pos`.  Out-of-range taps are clamped to the
 * buffer edges; the pre-fill/target logic keeps this path off the edges in
 * normal operation. */
static int32_t rs_sample(const int32_t *sb, size_t slen, int nch, int ch, double pos)
{
    if (slen < RS_TAPS + 2) return 0;
    long fi = (long)floor(pos);
    double frac = pos - (double)fi;
    long base = fi - (RS_TAPS / 2 - 1);
    double ph = frac * (double)RS_PHASES;
    int p0 = (int)ph;
    if (p0 < 0) p0 = 0;
    if (p0 >= RS_PHASES) p0 = RS_PHASES - 1;
    int p1 = (p0 + 1) % RS_PHASES;
    double pf = ph - floor(ph);
    double acc = 0.0;
    for (int t = 0; t < RS_TAPS; t++) {
        long idx = base + t;
        int32_t s;
        if (idx < 0) s = sb[ch];
        else if ((size_t)idx >= slen) s = sb[(slen - 1) * (size_t)nch + (size_t)ch];
        else s = sb[(size_t)idx * (size_t)nch + (size_t)ch];
        double coef = rs_coef[p0][t] * (1.0 - pf) + rs_coef[p1][t] * pf;
        acc += coef * (double)s;
    }
    long v = lrint(acc);
    if (v > 0x7fffff) v = 0x7fffff;
    if (v < -0x800000) v = -0x800000;
    return (int32_t)v;
}

int main(int argc, char **argv)
{
    cfg_t c;
    memset(&c, 0, sizeof c);
    c.in_port = 5004; c.out_port = 5004; c.pt = 96; c.ttl = 8;
    c.sr_interval = 1; c.ssrc = 0; c.rate = 48000; c.iface[0] = 0;
    c.bitdepth = 24; c.channels = 2; c.pkt = 48; c.raw = 0;
    c.jbuf = 32;   /* raw input ring capacity (packets); ~32 ms at 48k/48 */
    c.latency_ms = 0;   /* target buffer latency (ms); 0 = auto = half of jbuf */
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
        {"latency",      required_argument, 0, 'L'},
        {"verbose",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int copt;
    while ((copt = getopt_long(argc, argv, "A:P:S:a:p:k:t:l:f:d:r:RB:KWb:C:n:J:L:vh", opts, NULL)) != -1) {
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
        case 'L': c.latency_ms = atoi(optarg); break;
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
    int step_bad = 0;          /* consecutive PTP-step checks above threshold */

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
        int bps = c.bitdepth / 8, nch = c.channels > 0 ? c.channels : 2;
        int jb = c.jbuf > 0 ? c.jbuf : 32;

        /* Measure the grandmaster (PHC) rate relative to the system clock.  A strict
         * AES67 receiver plays the stream against the grandmaster clock and resamples
         * away any mismatch; if the content stays on the system clock the source's
         * media clock then drifts from the PHC and the receiver drops the stream
         * ("Late", buffer fills, no audio).  We emit at the grandmaster rate and
         * resample the system-rate content onto it with a fractional linear interpolator. */
        double phc_rate = 1.0;
        {
            struct timespec a0, a1, b0, b1;
            clock_gettime(CLOCK_MONOTONIC, &a0); clock_gettime(PHC_CLOCKID, &b0);
            usleep(4000000);
            clock_gettime(CLOCK_MONOTONIC, &a1); clock_gettime(PHC_CLOCKID, &b1);
            long long da = (a1.tv_sec - a0.tv_sec) * 1000000000LL + (a1.tv_nsec - a0.tv_nsec);
            long long db = (b1.tv_sec - b0.tv_sec) * 1000000000LL + (b1.tv_nsec - b0.tv_nsec);
            if (da > 0 && db > 0) phc_rate = (double)db / (double)da;
            if (!(phc_rate > 0.9995 && phc_rate < 1.0005)) phc_rate = 1.0;
        }
        if (c.verbose)
            fprintf(stderr, "  grandmaster rate %+.2f ppm vs system (resample ratio %.9f)\n",
                    (phc_rate - 1.0) * 1e6, phc_rate);

        /* Media-clock slot, in SYSTEM time, that keeps the emit running at the
         * grandmaster rate (a few ppm above/below the nominal 1 ms). */
        double pkt_period = (double)c.pkt / (double)c.rate;        /* 1 ms */
        double slot_period = pkt_period / phc_rate;                /* ~1.00001 ms */
        if (c.verbose)
            fprintf(stderr, "  raw: %d bytes (%d frames)/pkt @ %d Hz, paced %.5f ms, jitter %d pkts\n",
                    packet_bytes, c.pkt, c.rate, slot_period * 1000.0, jb);

        /* make stdin non-blocking so we can poll it against the send schedule */
        int f_orig = fcntl(0, F_GETFL, 0);
        fcntl(0, F_SETFL, f_orig | O_NONBLOCK);

        /* --- adaptive resampler: interleaved 32-bit source frames --- */
        size_t cap = (size_t)(jb + 8) * (size_t)c.pkt + (size_t)c.pkt + 8;
        int32_t *sb  = malloc(cap * (size_t)nch * sizeof *sb);
        unsigned char *ib = malloc((size_t)packet_bytes + 16);
        size_t slen = 0, igot = 0;                 /* buffered frames / undecoded bytes */
        int eof = 0;

        /* Target fill of the input ring.  Everything between the source and the
         * PTP-paced output is absorbed here: network jitter, decoder bursts and
         * the remote encoder's clock drift.  The PI controller below keeps this
         * fill constant by very slowly changing the resampling ratio. */
        int target_ms = c.latency_ms > 0
                            ? c.latency_ms
                            : (int)((long long)jb * c.pkt * 1000 / (2 * c.rate));
        if (target_ms < 20) target_ms = 20;
        size_t target_frames = (size_t)((double)target_ms * (double)c.rate / 1000.0);
        size_t margin = (size_t)(RS_TAPS + 2 * c.pkt);
        if (target_frames < margin) target_frames = margin;
        if (target_frames + margin > cap) target_frames = cap - margin;
        rs_init();

        /* Pre-fill to the target (+ resampler lookahead) before the first emit.
         * A bigger target means a slower start but a much more robust stream;
         * the service uses -L 1500 -J 4096 (~1.5 s target, 4 s capacity). */
        size_t prefill = target_frames + margin;
        if (prefill > cap) prefill = cap;
        int stall = 0;
        while (slen < prefill && !eof && !g_stop) {
            int r = (int)read(0, ib + igot, (size_t)packet_bytes - igot);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) {
                    if (++stall > 5000) break;      /* ~10 s without any input */
                    usleep(2000);
                    continue;
                }
                eof = 1;
                break;
            }
            if (r == 0) { eof = 1; break; }
            stall = 0;
            igot += (size_t)r;
            while (igot >= (size_t)bytes_per_frame && slen < cap) {
                unsigned char *f = ib;
                for (int ch = 0; ch < nch; ch++) {
                    int v = (int)(unsigned char)f[ch*bps]
                          | ((int)(unsigned char)f[ch*bps+1] << 8)
                          | ((int)(unsigned char)f[ch*bps+2] << 16);
                    if (v & 0x800000) v -= 0x1000000;               /* sign-extend 24-bit */
                    sb[(slen * nch) + ch] = v;
                }
                memmove(ib, ib + bytes_per_frame, igot - (size_t)bytes_per_frame);
                igot -= (size_t)bytes_per_frame;
                slen++;
            }
        }

        struct timespec nowt; clock_gettime(CLOCK_MONOTONIC, &nowt);
        double base_s = nowt.tv_sec + nowt.tv_nsec / 1e9;
        double base_phc = phc_sec(); if (base_phc < 0) base_phc = base_s;
        uint64_t slot = 0;
        /* Start a little into the buffer so the sinc has preceding taps. */
        double rpos = (double)(RS_TAPS / 2);        /* source-frame position (fractional) */
        double mts_acc = 0.0;                       /* fractional, PHC-seeded media clock */
        double corr_i = 0.0;                        /* integral part of the PI controller */
        unsigned long long underruns = 0;

        while (!g_stop) {
            /* pace each media-clock slot against the PHC (the grandmaster) so the media
             * clock tracks it exactly and never drifts from a once-measured value.  A
             * content burst is drained AFTER the emit below, so it cannot delay this. */
            clock_gettime(CLOCK_MONOTONIC, &nowt);
            double now_s = nowt.tv_sec + nowt.tv_nsec / 1e9;
            double phc_now = phc_sec(); if (phc_now < 0) phc_now = base_phc;
            double target_phc = base_phc + (double)slot * pkt_period;

            if (slen == 0 && eof) {
                if (c.verbose) fprintf(stderr, "input EOF (upstream stopped) - exiting so systemd restarts\n");
                break;
            }

            if (phc_now < target_phc) {
                /* sleep to the MONO time at which the PHC will reach target_phc; the
                 * re-check on phc_sec() below then fires the emit exactly at the PHC slot */
                double rem = target_phc - phc_now;
                double mono_target = now_s + rem / phc_rate;
                struct timespec ts;
                ts.tv_sec = (time_t)mono_target;
                ts.tv_nsec = (long)((mono_target - (double)ts.tv_sec) * 1e9);
                if (ts.tv_nsec > 999999999L) ts.tv_nsec = 999999999L;
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
                continue;                       /* phc_now is ~target_phc; emit on next pass */
            }

            /* a media-clock slot is due.  Emit at the grandmaster rate ALWAYS (filling an
             * upstream underrun with a zero packet and skipping the missing source frames)
             * so the media clock never stalls/drifts from the grandmaster, which is what
             * a strict receiver flags as "Late". */
            {
                /* restart the schedule if it genuinely fell behind (never burst) */
                if (phc_now - target_phc > pkt_period) { base_phc = phc_now; slot = 0; }
                /* The media clock advances by the exact sample count per packet, and the
                 * emit itself is paced at the PHC (grandmaster) slots above, so the rate is
                 * the grandmaster rate automatically — no measured-rate bias, even clock. */
                if (mts_acc == 0.0) {
                    double s0 = phc_sec(); if (s0 < 0) s0 = 0;
                    mts_acc = fmod(s0 * (double)c.rate, 4294967296.0);
                }
                double mts_delta = (double)c.pkt;
                mts_acc += mts_delta;

                /* PI controller: compare the input ring fill with the target and
                 * slowly adapt the resampling ratio.  Positive error (buffer too
                 * full) -> consume slightly more source frames per output frame,
                 * so the buffer drains back to target.  This compensates both the
                 * remote encoder's clock drift and any PTP/system rate offset. */
                double fill = (double)slen - rpos;
                double err = (fill - (double)target_frames) / (double)target_frames;
                double dt = (double)c.pkt / (double)c.rate;
                corr_i += 5e-4 * err * dt;
                if (corr_i > 0.01) corr_i = 0.01;
                if (corr_i < -0.01) corr_i = -0.01;
                double corr = 2e-4 * err + corr_i;
                if (corr > 0.01) corr = 0.01;
                if (corr < -0.01) corr = -0.01;
                double cstep = 1.0 + corr;              /* source frames per output frame */

                uint32_t ts = (uint32_t)mts_acc;
                media_ts = ts;
                out[0]=0x80; out[1]=(unsigned char)(c.pt & 0x7f);
                out[2]=(seq>>8)&0xff; out[3]=seq&0xff;
                out[4]=(ts>>24)&0xff; out[5]=(ts>>16)&0xff; out[6]=(ts>>8)&0xff; out[7]=ts&0xff;
                out[8]=(ssrc>>24)&0xff; out[9]=(ssrc>>16)&0xff; out[10]=(ssrc>>8)&0xff; out[11]=ssrc&0xff;

                long base = (long)floor(rpos + (double)(c.pkt - 1) * cstep)
                            - (RS_TAPS / 2 - 1);
                if (base < 0) base = 0;
                size_t need = (size_t)base + RS_TAPS + 1;
                int have = (slen >= need &&
                            fill > (double)c.pkt * cstep + (double)RS_TAPS + 2.0);
                if (have) {
                    for (int k = 0; k < c.pkt; k++) {
                        double fpos = rpos + (double)k * cstep;
                        for (int ch = 0; ch < nch; ch++) {
                            int32_t v = rs_sample(sb, slen, nch, ch, fpos);
                            size_t o = 12 + ((size_t)k * (size_t)nch + (size_t)ch) * (size_t)bps;
                            out[o]     = (unsigned char)((v >> 16) & 0xff);
                            out[o + 1] = (unsigned char)((v >> 8)  & 0xff);
                            out[o + 2] = (unsigned char)( v        & 0xff);
                        }
                    }
                    rpos += (double)c.pkt * cstep;
                } else {
                    /* Upstream underrun: keep the PTP media clock running with a
                     * zero packet and advance the source position, so a stall never
                     * turns into permanently growing latency. */
                    for (int k = 0; k < c.pkt * nch * bps; k++) out[12 + k] = 0;
                    underruns++;
                    rpos += (double)c.pkt;
                    if (rpos > (double)slen) rpos = (double)slen;
                }
                sendto(tx, out, 12 + packet_bytes, 0, (struct sockaddr *)&txa, sizeof txa);
                seq++; lastts = ts; pktcnt++; octet += packet_bytes;
                slot++;

                /* PTP-step detection.  The media clock is seeded from the PHC
                 * once and then advanced by the exact sample count; a PTP step
                 * (grandmaster reboot/reconfiguration, switch hiccup) leaves it
                 * at the old offset, which a Dante receiver sees as permanent
                 * "Late" packets.  Every 250 packets compare the absolute RTP
                 * timestamp with the PHC; two consecutive checks more than 3 ms
                 * apart mean a real step, so reseed the media clock in place.
                 * A short timestamp discontinuity is much better than a service
                 * restart (no Liquidsoap reconnect) and the receiver re-locks. */
                if ((pktcnt % 250) == 0) {
                    long diff = ptp_media_diff(c.rate, ts);
                    long lim = (long)(0.003 * (double)c.rate);
                    if (llabs(diff) > lim) {
                        if (++step_bad >= 2) {
                            double ps = phc_sec();
                            if (ps > 0) {
                                mts_acc = fmod(ps * (double)c.rate, 4294967296.0);
                                media_ts = (uint32_t)mts_acc;
                                base_phc = ps;
                                slot = 0;
                                step_bad = 0;
                                fprintf(stderr,
                                        "PTP step detected (%+.1f ms) - media clock reseeded\n",
                                        (double)diff * 1000.0 / (double)c.rate);
                            }
                        }
                    } else {
                        step_bad = 0;
                    }
                }

                /* Drop consumed frames, keeping RS_TAPS/2 frames of history for the
                 * sinc interpolator. */
                double keep = rpos - (double)(RS_TAPS / 2);
                if (keep > 0.0) {
                    size_t drop = (size_t)keep;
                    if (drop > slen) drop = slen;
                    memmove(sb, sb + drop * (size_t)nch,
                            (slen - drop) * (size_t)nch * sizeof *sb);
                    slen -= drop;
                    rpos -= (double)drop;
                }

                time_t now = time(NULL);
                if (c.sr_interval > 0 && now != lastSR) { lastSR = now; send_sr(tx,&txa,ssrc,pktcnt,octet,lastts); }
                if (c.verbose && now != lastLog) { lastLog = now;
                    double pc = phc_sec(); if (pc > 0) {
                        double phcts = fmod(pc * (double)c.rate, 4294967296.0);
                        double off = phcts - (double)ts;
                        if (off > 2147483648.0) off -= 4294967296.0;
                        if (off < -2147483648.0) off += 4294967296.0;
                        fprintf(stderr, "seq=%u ts=%u pkt=%llu fill=%.1fms target=%.1fms corr=%+.1fppm underruns=%llu offset_sample=%+.0f (%+.3f ms)\n",
                                seq, ts, (unsigned long long)pktcnt,
                                fill / (double)c.rate * 1000.0,
                                (double)target_frames / (double)c.rate * 1000.0,
                                corr * 1e6, underruns,
                                off, off / (double)c.rate * 1000.0);
                    }
                }
            }

            /* 3) decode up to one packet's worth of stdin into the buffer (bounded so
             *    a burst never delays the next emit -> keeps the timing even) */
            if (!eof && slen < cap) {
                int r = (int)read(0, ib + igot, (size_t)packet_bytes - igot);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN) eof = 1;
                } else if (r == 0) {
                    eof = 1;
                } else {
                    igot += (size_t)r;
                    while (igot >= (size_t)bytes_per_frame && slen < cap) {
                        unsigned char *f = ib;
                        for (int ch = 0; ch < nch; ch++) {
                            int v = (int)(unsigned char)f[ch*bps]
                                  | ((int)(unsigned char)f[ch*bps+1] << 8)
                                  | ((int)(unsigned char)f[ch*bps+2] << 16);
                            if (v & 0x800000) v -= 0x1000000;               /* sign-extend 24-bit */
                            sb[(slen * nch) + ch] = v;
                        }
                        memmove(ib, ib + bytes_per_frame, igot - (size_t)bytes_per_frame);
                        igot -= (size_t)bytes_per_frame;
                        slen++;
                    }
                }
            }
        }
        free(sb); free(ib);
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

            /* Same PTP-step detection for the relay path.  Only when we are
             * re-stamping (--keep-ts means the source's timestamps are not
             * necessarily PHC-referenced and must not be reseeded). */
            if (c.restamp && (pktcnt % 250) == 0) {
                long diff = ptp_media_diff(c.rate, lastts);
                long lim = (long)(0.003 * (double)c.rate);
                if (llabs(diff) > lim) {
                    if (++step_bad >= 2) {
                        double ps = phc_sec();
                        if (ps > 0) {
                            media_ts = (uint32_t)fmod(ps * (double)c.rate, 4294967296.0);
                            step_bad = 0;
                            fprintf(stderr,
                                    "PTP step detected (%+.1f ms) - media clock reseeded\n",
                                    (double)diff * 1000.0 / (double)c.rate);
                        }
                    }
                } else {
                    step_bad = 0;
                }
            }

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
