/*
 * main.c — entry point of the single `rtwd` binary.
 *
 *   rtwd                     run the control daemon (the LaunchDaemon invocation)
 *   rtwd scan|connect|...    CLI client: drive the running daemon over its socket
 *   rtwd --k*                low-level kext diagnostics (developer use)
 *
 * The daemon is the only process that talks to the kext while it runs; the CLI
 * sends it commands over /var/run/rtw88d.sock exactly like the menu-bar app
 * does, so two control paths can never drive the chip concurrently (the cause
 * of the historical "-kconnect while connected" system freeze). When no daemon
 * is running, the CLI falls back to driving the kext directly — safe, because
 * there is nothing to race against.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <net/if.h>
#include "rtw_hw.h"
#include "config.h"
#include "rtwd.h"
#include "../server/rtw88_abi.h"

/* ---- tiny flat-JSON readers (the daemon's replies are single-level) ------- */

/* extract a string value: ..."key":"value"... (JSON-unescape \" \\ \n) */
static int js_str(const char *line, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[n++] = (*p == 'n') ? '\n' : *p;
        } else out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return 1;
}
static int js_bool(const char *line, const char *key)   /* -1 absent, 0 false, 1 true */
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    return strncmp(p + strlen(pat), "true", 4) == 0 ? 1 : 0;
}
static long js_int(const char *line, const char *key, long dflt)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return dflt;
    return strtol(p + strlen(pat), NULL, 10);
}

/* ---- daemon socket client -------------------------------------------------- */

/* connect to the daemon socket; returns fd or -1 */
static int rtwd_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strlcpy(sa.sun_path, RTWD_SOCK_PATH, sizeof sa.sun_path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

static int rtwd_running(void)
{
    int fd = rtwd_connect();
    if (fd >= 0) { close(fd); return 1; }
    /* connect() can fail with ECONNREFUSED when the daemon is alive but its accept
     * backlog is full (busy with a long scan/connect). Distinguish that from "no
     * daemon" by the socket file's existence: if the bound socket is present, a
     * daemon owns it — don't fall back to racing it on the kext directly. */
    if (errno == ECONNREFUSED) {
        struct stat st;
        if (stat(RTWD_SOCK_PATH, &st) == 0 && S_ISSOCK(st.st_mode)) return 1;
    }
    return 0;
}

/* send one command line; invoke cb per reply line. Returns 0 on transport ok. */
static int rtwd_request(const char *line, void (*cb)(const char *, void *), void *ctx)
{
    int fd = rtwd_connect();
    if (fd < 0) return -1;
    char req[300];
    snprintf(req, sizeof req, "%s\n", line);
    send(fd, req, strlen(req), 0);

    char buf[4096]; size_t have = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + have, sizeof buf - 1 - have, 0);
        if (r <= 0) break;
        have += (size_t)r;
        buf[have] = 0;
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            if (cb && *start) cb(start, ctx);
            start = nl + 1;
        }
        have = strlen(start);
        memmove(buf, start, have + 1);
    }
    close(fd);
    return 0;
}

/* ---- CLI subcommands over the daemon -------------------------------------- */

struct scan_ctx { int rows; int failed; };

static void scan_line_cb(const char *line, void *vctx)
{
    struct scan_ctx *ctx = (struct scan_ctx *)vctx;
    char err[128];
    if (js_bool(line, "ok") == 0 && js_str(line, "error", err, sizeof err)) {
        fprintf(stderr, "scan failed: %s\n", err);
        ctx->failed = 1;
        return;
    }
    if (!strstr(line, "\"network\"")) return;
    char ssid[64] = "", bssid[24] = "", band[8] = "";
    js_str(line, "ssid", ssid, sizeof ssid);
    js_str(line, "bssid", bssid, sizeof bssid);
    js_str(line, "band", band, sizeof band);
    if (ctx->rows++ == 0) {
        printf("  %-32s  %-17s  band  ch  sec  saved  beacons\n", "SSID", "BSSID");
        printf("  --------------------------------  -----------------  ----  --  ---  -----  -------\n");
    }
    printf("  %-32s  %-17s  %-4s  %2ld  %-3s  %-5s  %ld\n",
           ssid, bssid, band, js_int(line, "channel", 0),
           js_bool(line, "secure") == 1 ? "yes" : "no",
           js_bool(line, "saved") == 1 ? "yes" : "no",
           js_int(line, "beacons", 0));
}

static int cli_scan(void)
{
    struct scan_ctx ctx = { 0, 0 };
    if (rtwd_request("scan", scan_line_cb, &ctx) != 0) return -1;
    if (ctx.failed) return 1;
    printf("%d network(s)\n", ctx.rows);
    return 0;
}

struct reply_ctx { char line[2048]; };
static void last_line_cb(const char *line, void *vctx)
{
    struct reply_ctx *ctx = (struct reply_ctx *)vctx;
    strlcpy(ctx->line, line, sizeof ctx->line);
}

/* one-reply commands: send, parse the single JSON reply, print outcome */
static int cli_simple(const char *cmd, const char *okmsg)
{
    struct reply_ctx ctx; ctx.line[0] = 0;
    if (rtwd_request(cmd, last_line_cb, &ctx) != 0) return -1;
    if (js_bool(ctx.line, "ok") == 1) { if (okmsg) printf("%s\n", okmsg); return 0; }
    char err[256] = "unknown error";
    js_str(ctx.line, "error", err, sizeof err);
    fprintf(stderr, "failed: %s\n", err);
    return 1;
}

static int cli_connect(const char *ssid, const char *pass, int nosave)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "connect\t%s\t%s%s", ssid, pass ? pass : "",
             nosave ? "\tnosave" : "");
    struct reply_ctx ctx; ctx.line[0] = 0;
    printf("connecting to \"%s\"...\n", ssid);
    if (rtwd_request(cmd, last_line_cb, &ctx) != 0) return -1;
    if (js_bool(ctx.line, "ok") == 1) {
        char ifname[32] = "";
        js_str(ctx.line, "ifname", ifname, sizeof ifname);
        printf("CONNECTED \xE2\x9C\x93  \"%s\" ch %ld (%s) — macOS will DHCP it.\n",
               ssid, js_int(ctx.line, "channel", 0), ifname[0] ? ifname : "enX");
        return 0;
    }
    char err[256] = "unknown error";
    js_str(ctx.line, "error", err, sizeof err);
    fprintf(stderr, "connect failed: %s\n", err);
    return 1;
}

static int cli_status(void)
{
    struct reply_ctx ctx; ctx.line[0] = 0;
    if (rtwd_request("status", last_line_cb, &ctx) != 0) return -1;
    if (js_bool(ctx.line, "ok") != 1) { fprintf(stderr, "status query failed\n"); return 1; }
    if (js_bool(ctx.line, "powered") == 0) { printf("radio: off\n"); return 0; }
    if (js_bool(ctx.line, "connected") == 1) {
        char ssid[64] = "", ifname[32] = "", ip[48] = "", band[8] = "";
        js_str(ctx.line, "ssid", ssid, sizeof ssid);
        js_str(ctx.line, "ifname", ifname, sizeof ifname);
        js_str(ctx.line, "ip", ip, sizeof ip);
        js_str(ctx.line, "band", band, sizeof band);
        printf("connected: \"%s\"  ch %ld (%sGHz)  %s%s%s\n",
               ssid, js_int(ctx.line, "channel", 0), band,
               ifname, ip[0] ? "  " : "", ip);
    } else {
        printf("not connected\n");
    }
    return 0;
}

/* ---- direct-kext fallback (no daemon running) ------------------------------ */

static void print_scan(const struct rtw_scan_result *sr)
{
    printf("  %-32s  %-17s  band  ch  sec  beacons\n", "SSID", "BSSID");
    printf("  --------------------------------  -----------------  ----  --  ---  -------\n");
    for (unsigned k = 0; k < sr->count; k++) {
        const struct rtw_scan_entry *e = &sr->nets[k];
        printf("  %-32s  %02x:%02x:%02x:%02x:%02x:%02x  %-4s  %2u  %-3s  %u\n",
               e->ssid, e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
               e->channel <= 14 ? "2.4" : "5", e->channel,
               e->privacy ? "yes" : "no", e->beacons);
    }
}

static int direct_scan(void)
{
    if (rtw_hw_open() != 0) return 1;
    struct rtw_scan_result sr;
    int n = hw_kctl_scan(&sr);
    rtw_hw_close();
    if (n < 0) { fprintf(stderr, "scan failed (is RTW88Server.kext loaded?)\n"); return 1; }
    printf("scan: %u network(s)\n", sr.count);
    print_scan(&sr);
    return 0;
}

static int direct_connect(const char *ssid, const char *pass)
{
    /* honor a saved password when one wasn't given (the daemon does the same via its
     * config; here we read the file ourselves so `rtwd connect SSID` works in the
     * no-daemon recovery path too — we're typically root there). */
    if (!pass || !pass[0]) {
        config_load_file(RTWD_CONF_PATH);
        const struct wifi_network *kn = network_match(ssid);
        if (kn && kn->password[0]) pass = kn->password;
    }
    if (rtw_hw_open() != 0) return 1;
    struct rtw_connect_result res;
    int rc = hw_kctl_connect(ssid, pass ? pass : "", &res);
    rtw_hw_close();
    if (rc != 0) { fprintf(stderr, "connect call failed\n"); return 1; }
    int found = res.status & 1, assoc = (res.status >> 1) & 1;
    int wpa = (res.status >> 2) & 1, dataup = (res.status >> 3) & 1;
    if (!(found && assoc && wpa && dataup)) {
        fprintf(stderr, "connect to \"%s\" did not complete (found=%d assoc=%d wpa=%d data=%d). "
                        "See dmesg `RTW88 kctl: connect`.\n", ssid, found, assoc, wpa, dataup);
        return 1;
    }
    printf("CONNECTED \xE2\x9C\x93  \"%s\" ch %u — 4-way + keys + media + data path, in-kernel.\n",
           ssid, res.channel);
    data_online(res.mac, NULL, 0);
    return 0;
}

static int direct_disconnect(void)
{
    if (rtw_hw_open() != 0) return 1;
    int rc = hw_kctl_disconnect();
    rtw_hw_close();
    printf("disconnect: %s\n", rc == 0 ? "OK — enX removed" : "call failed");
    return rc == 0 ? 0 : 1;
}

static int direct_status(void)
{
    if (rtw_hw_open() != 0) return 1;
    struct rtw_status_result st;
    int rc = hw_kctl_status(&st);
    rtw_hw_close();
    if (rc != 0) { fprintf(stderr, "status call failed\n"); return 1; }
    if (st.connected) {
        char ifname[IFNAMSIZ] = "", ip[48] = "";
        if (find_ifname(st.mac, ifname, sizeof ifname) == 0)
            iface_ipv4(ifname, ip, sizeof ip);
        printf("connected: \"%s\"  ch %u  %s%s%s\n", st.ssid, st.channel,
               ifname, ip[0] ? "  " : "", ip);
    } else printf("not connected\n");
    return 0;
}

/* ---- low-level kext diagnostics (developer use) ---------------------------- */

/* These bypass the daemon and drive the kext directly. Refuse while the daemon
 * is running (two control paths on one chip was the historical system-freeze),
 * unless --force is also given. */
static int diag_guard(int force)
{
    if (!rtwd_running()) return 0;
    if (force) {
        fprintf(stderr, "WARNING: rtwd is running; --force bypasses the safety check.\n");
        return 0;
    }
    fprintf(stderr,
        "refused: the rtwd daemon is running and owns the kext. Stop it first:\n"
        "    sudo launchctl bootout system/com.rtw88.rtwd\n"
        "or pass --force if you know the daemon is idle.\n");
    return -1;
}

static int diag_kpoweron(void)
{
    if (rtw_hw_open() != 0) return 1;
    uint32_t r = hw_kctl_poweron();
    int ret = (int)(int8_t)(r >> 16); unsigned cr = (r >> 8) & 0xff; int po = r & 1;
    printf("in-kernel power-on: ret=%d CR=0x%02x poweron=%d  -> %s\n",
           ret, cr, po, (cr == 0x00 && po) ? "OK (MAC up in-kernel) \xE2\x9C\x93" : "check dmesg");
    rtw_hw_close();
    return 0;
}

static int diag_kbringup(void)
{
    if (rtw_hw_open() != 0) return 1;
    uint32_t r = hw_kctl_bringup();
    int ok = (r >> 16) & 1; unsigned mcufw = r & 0xffff;
    printf("in-kernel bring-up: MCUFW_CTRL=0x%04x  -> %s\n  (see dmesg `RTW88 kctl: bringup`"
           " for per-stage ret + efuse MAC)\n",
           mcufw, ok ? "OK (firmware running, all stages passed) \xE2\x9C\x93" : "check dmesg");
    rtw_hw_close();
    return 0;
}

static int diag_kstats(void)
{
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

static void usage(const char *argv0)
{
    printf(
"usage: %s [command]\n"
"\n"
"With no command, runs the control daemon (the LaunchDaemon invocation).\n"
"\n"
"commands (sent to the running daemon; fall back to the kext when none runs):\n"
"  scan                       list visible networks\n"
"  connect SSID [PASS] [--nosave]\n"
"                             join a network (saved password reused if omitted;\n"
"                             --nosave skips remembering it)\n"
"  disconnect                 drop the connection (pauses auto-connect)\n"
"  status                     show the live connection state\n"
"  forget SSID                remove a saved network\n"
"  power on|off               radio master switch (persists across reboots)\n"
"\n"
"diagnostics (drive the kext directly; refused while the daemon runs):\n"
"  --kpoweron | --kbringup | --kscan | --kconnect SSID PASS | --kdisconnect\n"
"  --kstats                   poll the in-kernel data-path counters\n"
"  add --force to bypass the daemon-running check\n",
        argv0);
}

int main(int argc, char **argv)
{
    /* no arguments -> the daemon personality */
    if (argc <= 1) return rtwd_daemon_main();

    const char *cmd = argv[1];
    int force = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--force")) force = 1;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(argv[0]);
        return 0;
    }

    /* ---- diagnostics (direct kext access, daemon-guarded) ---- */
    if (!strncmp(cmd, "--k", 3)) {
        if (diag_guard(force) != 0) return 2;
        if (!strcmp(cmd, "--kpoweron"))    return diag_kpoweron();
        if (!strcmp(cmd, "--kbringup"))    return diag_kbringup();
        if (!strcmp(cmd, "--kscan"))       return direct_scan();
        if (!strcmp(cmd, "--kstats"))      return diag_kstats();
        if (!strcmp(cmd, "--kdisconnect")) return direct_disconnect();
        if (!strcmp(cmd, "--kconnect")) {
            if (argc < 4) { printf("usage: --kconnect SSID PASSWORD\n"); return 2; }
            return direct_connect(argv[2], argv[3]);
        }
        fprintf(stderr, "unknown diagnostic %s (try help)\n", cmd);
        return 2;
    }

    /* ---- normal commands: daemon first, direct-kext fallback ---- */
    int have_daemon = rtwd_running();

    if (!strcmp(cmd, "scan")) {
        if (have_daemon) return cli_scan();
        fprintf(stderr, "(rtwd not running — driving the kext directly)\n");
        return direct_scan();
    }
    if (!strcmp(cmd, "connect")) {
        if (argc < 3) { fprintf(stderr, "usage: %s connect SSID [PASS] [--nosave]\n", argv[0]); return 2; }
        const char *ssid = argv[2];
        const char *pass = (argc > 3 && strcmp(argv[3], "--nosave") != 0) ? argv[3] : NULL;
        int nosave = 0;
        for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--nosave")) nosave = 1;
        if (have_daemon) return cli_connect(ssid, pass, nosave);
        fprintf(stderr, "(rtwd not running — driving the kext directly)\n");
        return direct_connect(ssid, pass);
    }
    if (!strcmp(cmd, "disconnect")) {
        if (have_daemon) return cli_simple("disconnect", "disconnected");
        fprintf(stderr, "(rtwd not running — driving the kext directly)\n");
        return direct_disconnect();
    }
    if (!strcmp(cmd, "status")) {
        if (have_daemon) return cli_status();
        fprintf(stderr, "(rtwd not running — driving the kext directly)\n");
        return direct_status();
    }
    if (!strcmp(cmd, "forget")) {
        if (argc < 3) { fprintf(stderr, "usage: %s forget SSID\n", argv[0]); return 2; }
        if (!have_daemon) { fprintf(stderr, "forget needs the rtwd daemon (it owns the saved-network list)\n"); return 1; }
        char buf[300];
        snprintf(buf, sizeof buf, "forget\t%s", argv[2]);
        return cli_simple(buf, "forgotten");
    }
    if (!strcmp(cmd, "power")) {
        if (argc < 3) { fprintf(stderr, "usage: %s power on|off\n", argv[0]); return 2; }
        if (!have_daemon) { fprintf(stderr, "power needs the rtwd daemon\n"); return 1; }
        char buf[64];
        snprintf(buf, sizeof buf, "power\t%s", argv[2]);
        return cli_simple(buf, NULL) == 0 ? (printf("radio %s\n", argv[2]), 0) : 1;
    }

    fprintf(stderr, "unknown command: %s (try help)\n", cmd);
    return 2;
}
