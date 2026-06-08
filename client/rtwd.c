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
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rtw_hw.h"
#include "config.h"
#include "../server/rtw88_abi.h"

#define RTWD_SOCK_PATH "/var/run/rtw88d.sock"
#define RTWD_CONF_PATH "/usr/local/etc/rtw88.conf"

/* The kext is the source of truth for the live connection (see hw_kctl_status),
 * so rtwd holds almost no connection state of its own — only the policy bits:
 *   g_kext_lock : serialize kext access (rtw_hw_open/close uses a shared global
 *                 handle, so the accept loop and the auto-connect thread must not
 *                 overlap).
 *   g_user_off  : the GUI/CLI asked to disconnect on purpose — pause auto-connect
 *                 until the next explicit connect, so we don't instantly rejoin. */
static pthread_mutex_t g_kext_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_user_off  = 0;
static int             g_radio_off = 0;   /* radio powered off (GUI power switch) */

static void kext_lock(void)   { pthread_mutex_lock(&g_kext_lock); }
static void kext_unlock(void) { pthread_mutex_unlock(&g_kext_lock); }

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
/* send a full buffer to a client fd (used by the streaming scan). */
static void send_all(int fd, const char *s)
{
    size_t n = strlen(s), off = 0;
    while (off < n) { ssize_t w = send(fd, s + off, n - off, 0); if (w <= 0) break; off += (size_t)w; }
}

/* serialize one scan entry as a {"network":{...}} line into buf. */
static void net_line(char *buf, size_t cap, const struct rtw_scan_entry *e)
{
    size_t len = 0;
    appendf(buf, cap, &len, "{\"network\":{\"ssid\":");
    json_str(buf, cap, &len, e->ssid);
    appendf(buf, cap, &len,
            ",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"channel\":%u,\"band\":\"%s\",\"beacons\":%u,\"saved\":%s}}\n",
            e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
            e->channel, e->channel <= 14 ? "2.4" : "5", e->beacons,
            network_match(e->ssid) ? "true" : "false");
}

/* Streaming scan: scan the channel list a few channels at a time and emit each
 * newly-seen network as its own JSON line as soon as its chunk completes, then a
 * final {"done":true}. The GUI appends rows live instead of waiting ~11s. */
static void cmd_scan(int fd)
{
    static const unsigned char chans[] = {
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140, 149, 153, 157, 161, 165,                 /* 5 GHz */
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13               /* 2.4 GHz */
    };
    const int total = (int)sizeof(chans), CHUNK = 6;

    if (g_radio_off) { send_all(fd, "{\"ok\":false,\"error\":\"radio is off\"}\n"); return; }

    kext_lock();
    if (rtw_hw_open() != 0) { kext_unlock(); send_all(fd, "{\"ok\":false,\"error\":\"kext not reachable\"}\n"); return; }
    send_all(fd, "{\"ok\":true,\"scanning\":true}\n");

    uint8_t seen[128][6]; int nseen = 0;
    static struct rtw_scan_result sr;
    for (int off = 0; off < total; off += CHUNK) {
        struct rtw_scan_chans in;
        memset(&in, 0, sizeof in);
        in.flags = (off == 0 ? RTW_SCAN_F_BEGIN : 0) | (off + CHUNK >= total ? RTW_SCAN_F_END : 0);
        in.count = (off + CHUNK <= total) ? (uint32_t)CHUNK : (uint32_t)(total - off);
        memcpy(in.ch, chans + off, in.count);

        if (hw_kctl_scan_chunk(&in, &sr) < 0) continue;
        for (unsigned k = 0; k < sr.count; k++) {
            const struct rtw_scan_entry *e = &sr.nets[k];
            int dup = 0;
            for (int j = 0; j < nseen; j++) if (memcmp(seen[j], e->bssid, 6) == 0) { dup = 1; break; }
            if (dup) continue;
            if (nseen < 128) memcpy(seen[nseen++], e->bssid, 6);
            char line[512];
            net_line(line, sizeof line, e);
            send_all(fd, line);
        }
    }
    rtw_hw_close();
    kext_unlock();
    send_all(fd, "{\"ok\":true,\"done\":true}\n");
}

/* run an in-kernel connect under the kext lock. Fills *res; returns the
 * hw_kctl_connect rc (0 = call ok), or -2 if the kext isn't reachable. */
static int kconnect(const char *ssid, const char *pass, struct rtw_connect_result *res)
{
    memset(res, 0, sizeof *res);
    kext_lock();
    if (rtw_hw_open() != 0) { kext_unlock(); return -2; }
    int rc = hw_kctl_connect(ssid, pass ? pass : "", res);
    rtw_hw_close();
    kext_unlock();
    return rc;
}

static void cmd_connect(const char *ssid, const char *pass, char *out, size_t cap)
{
    if (!ssid || !ssid[0]) { snprintf(out, cap, "{\"ok\":false,\"error\":\"missing ssid\"}\n"); return; }
    if (g_radio_off)       { snprintf(out, cap, "{\"ok\":false,\"error\":\"radio is off\"}\n"); return; }

    /* no password given? fall back to a saved one for this SSID (known network). */
    const char *use_pass = (pass && pass[0]) ? pass : NULL;
    if (!use_pass) { const struct wifi_network *kn = network_match(ssid); if (kn) use_pass = kn->password; }

    g_user_off = 0;   /* an explicit connect re-arms auto-connect */

    struct rtw_connect_result res;
    int rc = kconnect(ssid, use_pass ? use_pass : "", &res);
    if (rc == -2) { snprintf(out, cap, "{\"ok\":false,\"error\":\"kext not reachable\"}\n"); return; }

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

    /* connected — persist this network so it reconnects password-free next time,
     * then bring enX online (DHCP runs outside the kext lock). */
    if (use_pass && use_pass[0]) {
        config_set_network(ssid, use_pass);
        config_save(RTWD_CONF_PATH);
    }
    char ifname[IFNAMSIZ] = "";
    data_online(res.mac, ifname, sizeof ifname);

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
    g_user_off = 1;   /* deliberate disconnect — don't let auto-connect undo it */
    kext_lock();
    if (rtw_hw_open() != 0) { kext_unlock(); snprintf(out, cap, "{\"ok\":false,\"error\":\"kext not reachable\"}\n"); return; }
    int rc = hw_kctl_disconnect();
    rtw_hw_close();
    kext_unlock();
    snprintf(out, cap, "{\"ok\":%s}\n", rc == 0 ? "true" : "false");
}

/* drop a saved network (so the GUI can "forget" it). */
static void cmd_forget(const char *ssid, char *out, size_t cap)
{
    if (!ssid || !ssid[0]) { snprintf(out, cap, "{\"ok\":false,\"error\":\"missing ssid\"}\n"); return; }
    int found = (config_forget_network(ssid) == 0);
    if (found) config_save(RTWD_CONF_PATH);
    snprintf(out, cap, "{\"ok\":%s}\n", found ? "true" : "false");
}

/* power the radio on/off (a Wi-Fi master switch, separate from connect). OFF
 * tears down any connection + halts the chip (trx_free disables bus-mastering)
 * and pauses auto-connect; ON re-arms auto-connect (which rejoins a known net). */
static void cmd_power(const char *arg, char *out, size_t cap)
{
    int on = arg && (!strcmp(arg, "on") || !strcmp(arg, "1") || !strcmp(arg, "true"));
    if (on) {
        g_radio_off = 0;
        g_user_off  = 0;   /* let auto-connect bring a known network back */
    } else {
        g_radio_off = 1;
        g_user_off  = 1;
        kext_lock();
        if (rtw_hw_open() == 0) { hw_kctl_disconnect(); rtw_hw_close(); }
        kext_unlock();
    }
    snprintf(out, cap, "{\"ok\":true,\"powered\":%s}\n", on ? "true" : "false");
}

static void cmd_status(char *out, size_t cap)
{
    /* the kext is authoritative: a CLI-initiated connect shows up here too. */
    struct rtw_status_result st;
    kext_lock();
    int ok = (rtw_hw_open() == 0);
    if (ok) { ok = (hw_kctl_status(&st) == 0); rtw_hw_close(); }
    kext_unlock();

    char ifname[IFNAMSIZ] = "", ip[INET_ADDRSTRLEN] = "";
    int connected = ok && st.connected;
    if (connected && find_ifname(st.mac, ifname, sizeof ifname) == 0)
        iface_ipv4(ifname, ip, sizeof ip);

    size_t len = 0;
    appendf(out, cap, &len, "{\"ok\":true,\"powered\":%s,\"connected\":%s",
            g_radio_off ? "false" : "true", connected ? "true" : "false");
    if (connected) {
        appendf(out, cap, &len, ",\"ssid\":");
        json_str(out, cap, &len, st.ssid);
        appendf(out, cap, &len, ",\"channel\":%d,\"band\":\"%s\",\"ifname\":",
                st.channel, st.channel <= 14 ? "2.4" : "5");
        json_str(out, cap, &len, ifname);
        appendf(out, cap, &len, ",\"ip\":");
        json_str(out, cap, &len, ip);
    }
    appendf(out, cap, &len, "}\n");
}

/* ---- auto-connect: join the strongest known network, keep the link up ----- *
 * Runs in its own thread (kext access serialized by g_kext_lock). On boot and
 * after any drop it scans and connects the best in-range SAVED network with no
 * prompt — unless the user deliberately disconnected (g_user_off). */
static void *auto_connect_thread(void *arg)
{
    (void)arg;
    int delay = 3;   /* first attempt shortly after boot */
    int fails = 0;   /* consecutive failed attempts -> exponential backoff */
    for (;;) {
        sleep(delay);
        delay = 15;
        if (g_user_off || g_radio_off || g_cfg.n_nets == 0) { fails = 0; continue; }

        /* already connected? (kext is the source of truth) */
        struct rtw_status_result st;
        kext_lock();
        int ok = (rtw_hw_open() == 0);
        if (ok) { ok = (hw_kctl_status(&st) == 0); rtw_hw_close(); }
        kext_unlock();
        if (ok && st.connected) { fails = 0; continue; }

        /* scan, then pick the strongest in-range saved network */
        static struct rtw_scan_result sr;
        int n = -1;
        kext_lock();
        if (rtw_hw_open() == 0) { n = hw_kctl_scan(&sr); rtw_hw_close(); }
        kext_unlock();
        if (n <= 0) continue;

        const struct wifi_network *best = NULL; unsigned best_beacons = 0;
        for (unsigned k = 0; k < sr.count; k++) {
            const struct rtw_scan_entry *e = &sr.nets[k];
            const struct wifi_network *kn = network_match(e->ssid);
            if (kn && e->beacons >= best_beacons) { best_beacons = e->beacons; best = kn; }
        }
        if (!best || g_user_off) continue;   /* re-check g_user_off: user may have acted during the scan */

        struct rtw_connect_result res;
        int rc = kconnect(best->ssid, best->password, &res);
        if (rc == 0 && ((res.status >> 3) & 1)) {
            char ifname[IFNAMSIZ] = "";
            data_online(res.mac, ifname, sizeof ifname);
            fprintf(stderr, "rtwd: auto-connected to %s (%s)\n", best->ssid, ifname);
            fails = 0;
        } else {
            /* back off a network that keeps failing (e.g. wrong saved password) so we
             * don't churn the chip's power-on/DMA-ring setup every 15s: 30s, 60s, 120s. */
            if (fails < 8) fails++;
            int mult = fails < 3 ? (1 << fails) : 8;     /* 2,4,8,8,... */
            delay = 15 * mult; if (delay > 120) delay = 120;
            fprintf(stderr, "rtwd: auto-connect to %s failed (status 0x%x); retry in %ds\n",
                    best->ssid, res.status, delay);
        }
    }
    return NULL;
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

    /* scan streams multiple JSON lines straight to the socket as chunks complete. */
    if (!strcmp(verb, "scan")) { cmd_scan(fd); return; }

    static char out[4096];
    out[0] = '\0';
    if      (!strcmp(verb, "connect"))    cmd_connect(a1, a2, out, sizeof out);
    else if (!strcmp(verb, "disconnect")) cmd_disconnect(out, sizeof out);
    else if (!strcmp(verb, "status"))     cmd_status(out, sizeof out);
    else if (!strcmp(verb, "forget"))     cmd_forget(a1, out, sizeof out);
    else if (!strcmp(verb, "power"))      cmd_power(a1, out, sizeof out);
    else snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"unknown command\"}\n");

    send_all(fd, out);
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

    /* load saved networks, then auto-connect to the strongest known one on boot. */
    config_load_file(RTWD_CONF_PATH);
    pthread_t auto_t;
    if (pthread_create(&auto_t, NULL, auto_connect_thread, NULL) == 0)
        pthread_detach(auto_t);

    fprintf(stderr, "rtwd: listening on %s (%d known network(s))\n", RTWD_SOCK_PATH, g_cfg.n_nets);
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
