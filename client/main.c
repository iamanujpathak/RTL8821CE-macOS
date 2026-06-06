/*
 * main.c — RTW88Client: a thin userspace control utility for RTW88Server.kext.
 *
 * The kext holds the full driver — power-on, firmware download, PHY/RF, scan,
 * 802.11 auth/association, the WPA2 4-way handshake + hardware CCMP, and the
 * in-kernel TX/RX data path. This utility just drives it over the user-client
 * ABI (scan / connect / disconnect / stats) and brings the resulting enX online
 * (find it by MAC, hand it to configd for DHCP — L3 is configd's job).
 *
 * Default (config/CLI) flow: scan, pick the strongest configured network in
 * range, connect, bring enX online, then idle. The explicit --k* subcommands
 * expose the individual kext operations for diagnostics.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include "rtw_hw.h"
#include "config.h"
#include "../server/rtw88_abi.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ---- enX bring-up (the only userspace step left; L3 is configd's job) ----- */

/* find the BSD name (enX) the kext gave our nub, matched by MAC */
static int find_ifname(const uint8_t *mac, char *out, size_t outsz)
{
    struct ifaddrs *ifa, *p; int found = 0;
    if (getifaddrs(&ifa)) return -1;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
        struct sockaddr_dl *dl = (struct sockaddr_dl *)p->ifa_addr;
        if (dl->sdl_alen == 6 && memcmp(LLADDR(dl), mac, 6) == 0) {
            strlcpy(out, p->ifa_name, outsz); found = 1; break;
        }
    }
    freeifaddrs(ifa);
    return found ? 0 : -1;
}

static const char *find_netcfg(void)
{
    static const char *cands[] = {
        "/usr/local/libexec/rtw88-eth-netcfg.sh",
        "dist/rtw88-eth-netcfg.sh", "../dist/rtw88-eth-netcfg.sh",
        "./rtw88-eth-netcfg.sh", NULL };
    for (int i = 0; cands[i]; i++) if (access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

/* after the kext connects + starts the in-kernel data path, find the card's enX
 * (by MAC) and hand it to configd for DHCP — the only userspace step left. */
void data_online(const uint8_t *mac)
{
    char ifname[IFNAMSIZ] = "enX";
    for (int i = 0; i < 40 && find_ifname(mac, ifname, sizeof ifname) != 0; i++)
        usleep(100000);
    printf("  %s UP (MAC %02x:%02x:%02x:%02x:%02x:%02x) — kext moves all TX/RX in-kernel.\n",
           ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const char *netcfg = find_netcfg();
    if (netcfg) {
        char c[256];
        snprintf(c, sizeof c, "bash %s %s dhcp", netcfg, ifname);
        int rc = system(c); (void)rc;
        printf("  %s set to DHCP (primary service). Give configd ~5s, then ping/browse.\n", ifname);
    } else {
        printf("  netcfg script not found; run `sudo ipconfig set %s DHCP` to get online.\n", ifname);
    }
}

/* ---- helpers shared by the config-driven flow -------------------------- */

static void print_scan(const struct rtw_scan_result *sr)
{
    printf("  %-32s  %-17s  band  ch  beacons\n", "SSID", "BSSID");
    printf("  --------------------------------  -----------------  ----  --  -------\n");
    for (unsigned k = 0; k < sr->count; k++) {
        const struct rtw_scan_entry *e = &sr->nets[k];
        printf("  %-32s  %02x:%02x:%02x:%02x:%02x:%02x  %-4s  %2u  %u\n",
               e->ssid, e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
               e->channel <= 14 ? "2.4" : "5", e->channel, e->beacons);
    }
}

/* among the scanned networks we have credentials for, pick the strongest
 * (most beacons heard). Returns its config entry, or NULL if none in range. */
static const struct wifi_network *best_known(const struct rtw_scan_result *sr)
{
    const struct wifi_network *best = NULL; unsigned best_beacons = 0;
    for (unsigned k = 0; k < sr->count; k++) {
        const struct wifi_network *net = network_match(sr->nets[k].ssid);
        if (net && sr->nets[k].beacons >= best_beacons) {
            best = net; best_beacons = sr->nets[k].beacons;
        }
    }
    return best;
}

int main(int argc, char **argv)
{
    /* ---- explicit diagnostics: drive one kext operation, then exit ------ */

    /* --kpoweron: run the power-on sequence in-kernel (kext executes it via
     * kBridgeKInit). CR=0x00 + poweron=1 means the MAC came up in-kernel. */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--kpoweron")) {
        if (rtw_hw_open() != 0) return 1;
        uint32_t r = hw_kctl_poweron();
        int ret = (int)(int8_t)(r >> 16); unsigned cr = (r >> 8) & 0xff; int po = r & 1;
        printf("in-kernel power-on: ret=%d CR=0x%02x poweron=%d  -> %s\n",
               ret, cr, po, (cr == 0x00 && po) ? "OK (MAC up in-kernel) \xE2\x9C\x93" : "check dmesg");
        rtw_hw_close();
        return 0;
    }
    /* --kbringup: full in-kernel bring-up (power -> rings -> fw -> mac_init -> efuse -> phy). */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--kbringup")) {
        if (rtw_hw_open() != 0) return 1;
        uint32_t r = hw_kctl_bringup();
        int ok = (r >> 16) & 1; unsigned mcufw = r & 0xffff;
        printf("in-kernel bring-up: MCUFW_CTRL=0x%04x  -> %s\n  (see dmesg `RTW88 kctl: bringup`"
               " for per-stage ret + efuse MAC)\n",
               mcufw, ok ? "OK (firmware running, all stages passed) \xE2\x9C\x93" : "check dmesg");
        rtw_hw_close();
        return 0;
    }
    /* --kscan: in-kernel bring-up + scan; the kext returns the network list. */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--kscan")) {
        if (rtw_hw_open() != 0) return 1;
        struct rtw_scan_result sr;
        int n = hw_kctl_scan(&sr);
        if (n < 0) { printf("in-kernel scan: call failed\n"); rtw_hw_close(); return 1; }
        printf("in-kernel scan: %u networks\n", sr.count);
        print_scan(&sr);
        rtw_hw_close();
        return 0;
    }
    /* --kconnect SSID PASS: full in-kernel connect. */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--kconnect")) {
        if (i + 2 >= argc) { printf("usage: --kconnect SSID PASSWORD\n"); return 2; }
        if (rtw_hw_open() != 0) return 1;
        struct rtw_connect_result res;
        if (hw_kctl_connect(argv[i + 1], argv[i + 2], &res) != 0) {
            printf("in-kernel connect: call failed\n"); rtw_hw_close(); return 1;
        }
        int found = res.status & 1, assoc = (res.status >> 1) & 1;
        int wpa = (res.status >> 2) & 1, dataup = (res.status >> 3) & 1;
        printf("in-kernel connect \"%s\": found=%d associated=%d wpa=%d data=%d ch=%u  -> %s\n",
               argv[i + 1], found, assoc, wpa, dataup, res.channel,
               (found && assoc && wpa) ? "CONNECTED (4-way + keys + media, in-kernel) \xE2\x9C\x93"
               : !found ? "SSID not found in scan" : "check dmesg `RTW88 kctl: connect`");
        if (found && assoc && wpa && dataup)
            data_online(res.mac);
        rtw_hw_close();
        return 0;
    }
    /* --kstats: poll the in-kernel data-path counters (run while browsing). */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--kstats")) {
        if (rtw_hw_open() != 0) return 1;
        printf("polling kext data-path stats every 1s (Ctrl-C to stop)...\n");
        hw_data_stats((uint64_t[21]){0});   /* prime: clear accumulated counters */
        for (;;) {
            sleep(1);
            uint64_t s[21]; hw_data_stats(s);
            unsigned long long rx = s[3], miss = s[13];
            double lossp  = (rx + miss) ? 100.0 * (double)miss / (double)(rx + miss) : 0.0;
            double avgmcs = s[16] ? (double)s[15] / (double)s[16] : 0.0;
            int tids = __builtin_popcount((unsigned)s[19]);
            printf("[kext] down %.2f / up %.2f Mbps | rx %llu retry %.0f%% | "
                   "LOSS %.0f%% (miss %llu, jump %llu) | ringFull %llu maxDepth %llu | "
                   "rate avg %.0f max %llu | retryDup %llu | tids %d | err %llu pdrop %llu\n",
                   (double)s[4] * 8 / 1e6, (double)s[2] * 8 / 1e6,
                   rx, rx ? 100.0 * (double)s[5] / (double)rx : 0.0,
                   lossp, miss, (unsigned long long)s[20],
                   (unsigned long long)s[14], (unsigned long long)s[7],
                   avgmcs, (unsigned long long)s[17], (unsigned long long)s[18],
                   tids, (unsigned long long)s[9], (unsigned long long)s[10]);
            fflush(stdout);
        }
    }
    /* --kdisconnect: tear down the in-kernel connection. */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--kdisconnect")) {
        if (rtw_hw_open() != 0) return 1;
        int rc = hw_kctl_disconnect();
        printf("in-kernel disconnect: %s — enX removed; macOS will use other interfaces.\n",
               rc == 0 ? "OK" : "call failed");
        rtw_hw_close();
        return rc == 0 ? 0 : 1;
    }

    /* ---- default: config/CLI-driven connect via the kext ---------------- */
    int pa = parse_args(argc, argv);
    if (pa != 0) return pa < 0 ? 2 : 0;

    if (rtw_hw_open() != 0)
        return 1;

    uint16_t ven = 0, dev = 0; uint32_t barlen = 0;
    rtw_hw_ident(&ven, &dev, &barlen);
    printf("=== RTW88Client — RTL8821CE control utility (kext owns the driver) ===\n");
    printf("bridge: PCI %04x:%04x BAR=0x%x\n", ven, dev, barlen);
    if (g_cfg.n_nets) {
        printf("known networks (%d):", g_cfg.n_nets);
        for (int i = 0; i < g_cfg.n_nets; i++) printf(" \"%s\"", g_cfg.nets[i].ssid);
        printf("\n");
    }

    /* scan (kext brings the chip up + scans), then pick the strongest network
     * we have credentials for. */
    struct rtw_scan_result sr;
    if (hw_kctl_scan(&sr) < 0) {
        fprintf(stderr, "scan failed (is the RTW88Server kext loaded?)\n");
        rtw_hw_close();
        return 1;
    }
    printf("scan: %u networks\n", sr.count);
    print_scan(&sr);

    if (g_cfg.mode == MODE_SCAN) { rtw_hw_close(); return 0; }

    const struct wifi_network *net = best_known(&sr);
    if (!net) {
        fprintf(stderr, "none of the %d configured network(s) are in range; nothing to join.\n",
                g_cfg.n_nets);
        rtw_hw_close();
        return 1;
    }

    /* connect in-kernel: the kext does auth + assoc + WPA2 4-way + key install +
     * media-connect and starts its in-kernel data path (publishes enX). */
    printf("\njoining \"%s\" (strongest known network in range)\n", net->ssid);
    struct rtw_connect_result res;
    if (hw_kctl_connect(net->ssid, net->password, &res) != 0) {
        fprintf(stderr, "connect call failed\n"); rtw_hw_close(); return 1;
    }
    int found = res.status & 1, assoc = (res.status >> 1) & 1;
    int wpa = (res.status >> 2) & 1, dataup = (res.status >> 3) & 1;
    if (!(found && assoc && wpa && dataup)) {
        fprintf(stderr, "connect to \"%s\" did not complete (found=%d assoc=%d wpa=%d data=%d). "
                        "See dmesg `RTW88 kctl: connect`.\n", net->ssid, found, assoc, wpa, dataup);
        rtw_hw_close();
        return 1;
    }
    printf("CONNECTED \xE2\x9C\x93  \"%s\" ch %u — 4-way + keys + media + data path, in-kernel.\n",
           net->ssid, res.channel);

    /* bring the kext's enX online (find by MAC + configd DHCP), then idle — the
     * kext owns the link; we stay resident so the LaunchDaemon considers us up. */
    data_online(res.mac);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!g_stop) pause();

    rtw_hw_close();
    return 0;
}
