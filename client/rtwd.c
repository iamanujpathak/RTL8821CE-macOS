/*
 * rtwd.c — RTW88 control daemon (HeliPort-style backend).
 *
 * A small root daemon that fronts the RTW88Server kext for an unprivileged GUI.
 * The kext user-client + DHCP need root, but a menu-bar app runs as the console
 * user — so this daemon (a LaunchDaemon) does the privileged work and exposes a
 * tiny line-based JSON protocol over a Unix socket. The GUI connects, sends one
 * command, reads one JSON reply, and closes.
 *
 * Reuses the exact same kext ABI (rtw_hw.c) + enX-online step the CLI uses, so
 * there is no new radio logic here — just IPC. Testable without a GUI:
 *     echo scan | nc -U /var/run/rtw88d.sock
 *     printf 'connect\tMySSID\tMyPass\n' | nc -U /var/run/rtw88d.sock
 *     echo status | nc -U /var/run/rtw88d.sock
 *
 * Protocol (request = one line, TAB-separated; reply = one JSON line):
 *   scan                         -> {"ok":true,"networks":[{ssid,bssid,channel,band,beacons}...]}
 *   connect <TAB> ssid <TAB> pass-> {"ok":true,"status":N,"ifname":"enX","mac":"..","channel":N}
 *   disconnect                   -> {"ok":true}
 *   status                       -> {"ok":true,"connected":B,"ssid":"..","ifname":"..","ip":"..","channel":N}
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rtw_hw.h"
#include "../server/rtw88_abi.h"

#define RTWD_SOCK_PATH "/var/run/rtw88d.sock"

/* last successful connection, so `status` can describe it (the kext doesn't keep SSID). */
static char    g_ssid[33] = "";
static uint8_t g_mac[6]    = {0};
static int     g_channel   = 0;
static int     g_connected = 0;

/* ---- enX bring-up (identical to the CLI's userspace step) ----------------- */
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

/* IPv4 address of an interface, or "" if none yet (DHCP still in progress). */
static void iface_ipv4(const char *ifname, char *out, size_t outsz)
{
    out[0] = '\0';
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa)) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, ifname) != 0) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outsz);
        break;
    }
    freeifaddrs(ifa);
}

static const char *find_netcfg(void)
{
    static const char *cands[] = {
        "/usr/local/libexec/rtw88-eth-netcfg.sh",
        "/usr/local/bin/rtw88-eth-netcfg.sh", NULL };
    for (int i = 0; cands[i]; i++) if (access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

/* find the card's enX (by MAC) and hand it to configd for DHCP. */
static void data_online(const uint8_t *mac, char *ifname_out, size_t outsz)
{
    char ifname[IFNAMSIZ] = "";
    for (int i = 0; i < 40 && find_ifname(mac, ifname, sizeof ifname) != 0; i++)
        usleep(100000);
    if (ifname[0]) {
        const char *netcfg = find_netcfg();
        if (netcfg) {
            char c[256];
            snprintf(c, sizeof c, "bash %s %s dhcp", netcfg, ifname);
            int rc = system(c); (void)rc;
        } else {
            char c[128];
            snprintf(c, sizeof c, "ipconfig set %s DHCP", ifname);
            int rc = system(c); (void)rc;
        }
    }
    if (ifname_out) strlcpy(ifname_out, ifname, outsz);
}

/* ---- minimal JSON output helpers ----------------------------------------- */
/* append a JSON-escaped string (quotes, backslash, control chars) to a buffer. */
static void json_str(char *dst, size_t cap, size_t *len, const char *s)
{
    size_t n = *len;
    if (n < cap) dst[n++] = '"';
    for (; *s && n + 2 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = (char)c; }
        else if (c == '\n') { dst[n++] = '\\'; if (n < cap) dst[n++] = 'n'; }
        else if (c < 0x20)  { /* drop other control chars */ }
        else dst[n++] = (char)c;
    }
    if (n < cap) dst[n++] = '"';
    *len = n;
}
static void appendf(char *dst, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (*len < cap) *len += vsnprintf(dst + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (*len > cap) *len = cap;
}

/* ---- command handlers (write a JSON reply line into `out`) ---------------- */
static void cmd_scan(char *out, size_t cap)
{
    if (rtw_hw_open() != 0) { snprintf(out, cap, "{\"ok\":false,\"error\":\"kext not reachable (loaded? root?)\"}\n"); return; }
    static struct rtw_scan_result sr;   /* large — keep off the stack */
    int n = hw_kctl_scan(&sr);
    rtw_hw_close();
    if (n < 0) { snprintf(out, cap, "{\"ok\":false,\"error\":\"scan failed\"}\n"); return; }

    size_t len = 0;
    appendf(out, cap, &len, "{\"ok\":true,\"networks\":[");
    for (unsigned k = 0; k < sr.count; k++) {
        const struct rtw_scan_entry *e = &sr.nets[k];
        if (k) appendf(out, cap, &len, ",");
        appendf(out, cap, &len, "{\"ssid\":");
        json_str(out, cap, &len, e->ssid);
        appendf(out, cap, &len,
                ",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"channel\":%u,\"band\":\"%s\",\"beacons\":%u}",
                e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
                e->channel, e->channel <= 14 ? "2.4" : "5", e->beacons);
    }
    appendf(out, cap, &len, "]}\n");
}

static void cmd_connect(const char *ssid, const char *pass, char *out, size_t cap)
{
    if (!ssid || !ssid[0]) { snprintf(out, cap, "{\"ok\":false,\"error\":\"missing ssid\"}\n"); return; }
    if (rtw_hw_open() != 0) { snprintf(out, cap, "{\"ok\":false,\"error\":\"kext not reachable\"}\n"); return; }
    struct rtw_connect_result res;
    memset(&res, 0, sizeof res);
    int rc = hw_kctl_connect(ssid, pass ? pass : "", &res);
    rtw_hw_close();

    int associated = (res.status >> 1) & 1, wpa = (res.status >> 2) & 1, data_up = (res.status >> 3) & 1;
    if (rc != 0 || !data_up) {
        size_t len = 0;
        appendf(out, cap, &len, "{\"ok\":false,\"status\":%u,\"associated\":%s,\"wpa\":%s,\"error\":",
                res.status, associated ? "true" : "false", wpa ? "true" : "false");
        json_str(out, cap, &len,
                 !(res.status & 1) ? "SSID not found in scan" :
                 !associated ? "association failed" :
                 !wpa ? "WPA handshake failed (wrong password?)" : "data path did not start");
        appendf(out, cap, &len, "}\n");
        return;
    }

    /* connected — bring enX online + remember it for status. */
    char ifname[IFNAMSIZ] = "";
    data_online(res.mac, ifname, sizeof ifname);
    strlcpy(g_ssid, ssid, sizeof g_ssid);
    memcpy(g_mac, res.mac, 6);
    g_channel = res.channel;
    g_connected = 1;

    size_t len = 0;
    appendf(out, cap, &len, "{\"ok\":true,\"status\":%u,\"channel\":%u,\"ifname\":", res.status, res.channel);
    json_str(out, cap, &len, ifname);
    appendf(out, cap, &len, ",\"ssid\":");
    json_str(out, cap, &len, ssid);
    appendf(out, cap, &len, ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",
            res.mac[0], res.mac[1], res.mac[2], res.mac[3], res.mac[4], res.mac[5]);
}

static void cmd_disconnect(char *out, size_t cap)
{
    if (rtw_hw_open() != 0) { snprintf(out, cap, "{\"ok\":false,\"error\":\"kext not reachable\"}\n"); return; }
    int rc = hw_kctl_disconnect();
    rtw_hw_close();
    g_connected = 0; g_ssid[0] = '\0';
    snprintf(out, cap, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
}

static void cmd_status(char *out, size_t cap)
{
    char ifname[IFNAMSIZ] = "", ip[INET_ADDRSTRLEN] = "";
    if (g_connected && find_ifname(g_mac, ifname, sizeof ifname) == 0)
        iface_ipv4(ifname, ip, sizeof ip);
    else
        g_connected = 0;   /* enX gone -> not connected */

    size_t len = 0;
    appendf(out, cap, &len, "{\"ok\":true,\"connected\":%s", g_connected ? "true" : "false");
    if (g_connected) {
        appendf(out, cap, &len, ",\"ssid\":");
        json_str(out, cap, &len, g_ssid);
        appendf(out, cap, &len, ",\"channel\":%d,\"band\":\"%s\",\"ifname\":",
                g_channel, g_channel <= 14 ? "2.4" : "5");
        json_str(out, cap, &len, ifname);
        appendf(out, cap, &len, ",\"ip\":");
        json_str(out, cap, &len, ip);
    }
    appendf(out, cap, &len, "}\n");
}

/* ---- one client connection: read a command line, reply, close ------------ */
static void handle_client(int fd)
{
    char req[256];
    ssize_t r = recv(fd, req, sizeof req - 1, 0);
    if (r <= 0) return;
    req[r] = '\0';
    char *nl = strpbrk(req, "\r\n"); if (nl) *nl = '\0';

    /* split on TAB: verb [ssid] [pass] */
    char *verb = req, *a1 = NULL, *a2 = NULL;
    char *t = strchr(req, '\t');
    if (t) { *t = '\0'; a1 = t + 1; char *t2 = strchr(a1, '\t'); if (t2) { *t2 = '\0'; a2 = t2 + 1; } }

    static char out[64 * 1024];   /* scan can be large */
    out[0] = '\0';
    if      (!strcmp(verb, "scan"))       cmd_scan(out, sizeof out);
    else if (!strcmp(verb, "connect"))    cmd_connect(a1, a2, out, sizeof out);
    else if (!strcmp(verb, "disconnect")) cmd_disconnect(out, sizeof out);
    else if (!strcmp(verb, "status"))     cmd_status(out, sizeof out);
    else snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"unknown command\"}\n");

    size_t n = strlen(out), off = 0;
    while (off < n) {
        ssize_t w = send(fd, out + off, n - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);

    unlink(RTWD_SOCK_PATH);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strlcpy(sa.sun_path, RTWD_SOCK_PATH, sizeof sa.sun_path);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); return 1; }
    /* the menu-bar app runs as the console user; allow it to connect. Single-user
     * Hackintosh box — acceptable. Tighten to a group with chown if desired. */
    chmod(RTWD_SOCK_PATH, 0666);
    if (listen(s, 8) != 0) { perror("listen"); return 1; }

    fprintf(stderr, "rtwd: listening on %s\n", RTWD_SOCK_PATH);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        handle_client(c);
        close(c);
    }
    close(s);
    unlink(RTWD_SOCK_PATH);
    return 0;
}
