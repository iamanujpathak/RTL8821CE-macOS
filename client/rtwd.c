/*
 * rtwd.c — RTW88 control daemon (HeliPort-style backend).
 *
 * The daemon personality of the single `rtwd` binary (main.c dispatches here when
 * run with no arguments — the LaunchDaemon invocation). A small root daemon that
 * fronts the RTW88Server kext for an unprivileged GUI/CLI: the kext user client +
 * DHCP need root, so this daemon does the privileged work and exposes a tiny
 * line-based JSON protocol over a Unix socket. It is the ONLY process that talks
 * to the kext while it runs — the CLI and the menu-bar app both go through it,
 * which is what prevents two control paths from driving the chip concurrently.
 *
 * Testable without a GUI:
 *     echo scan | nc -U /var/run/rtw88d.sock
 *     printf 'connect\tMySSID\tMyPass\n' | nc -U /var/run/rtw88d.sock
 *     echo status | nc -U /var/run/rtw88d.sock
 *
 * Protocol (request = one line, TAB-separated; reply = one or more JSON lines):
 *   scan                                -> streamed {"network":{...}} lines + {"done":true}
 *   connect <TAB> ssid [<TAB> pass [<TAB> nosave]]
 *                                       -> {"ok":true,"status":N,"ifname":"enX",...}
 *   disconnect                          -> {"ok":true}
 *   forget <TAB> ssid                   -> {"ok":true}
 *   power <TAB> on|off                  -> {"ok":true,"powered":B}
 *   status                              -> {"ok":true,"connected":B,"ssid":..,"ip":..}
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ucred.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rtw_hw.h"
#include "config.h"
#include "rtwd.h"
#include "../server/rtw88_abi.h"

/* The kext is the source of truth for the live connection (see hw_kctl_status),
 * so rtwd holds almost no connection state of its own — only the policy bits:
 *   g_kext_lock : serialize kext access (rtw_hw_open/close uses a shared global
 *                 handle, so the accept loop and the auto-connect thread must not
 *                 overlap).
 *   g_cfg_lock  : guard g_cfg (saved networks) — handle_client mutates it
 *                 (connect saves, forget removes) while auto_connect reads it.
 *   g_user_off  : the GUI/CLI asked to disconnect on purpose — pause auto-connect
 *                 until the next explicit connect, so we don't instantly rejoin. */
static pthread_mutex_t g_kext_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cfg_lock  = PTHREAD_MUTEX_INITIALIZER;
static int             g_user_off  = 0;
static int             g_radio_off = 0;   /* radio powered off (GUI power switch) */

static void kext_lock(void)   { pthread_mutex_lock(&g_kext_lock); }
static void kext_unlock(void) { pthread_mutex_unlock(&g_kext_lock); }
static void cfg_lock(void)    { pthread_mutex_lock(&g_cfg_lock); }
static void cfg_unlock(void)  { pthread_mutex_unlock(&g_cfg_lock); }

/* ---- enX bring-up (shared with the CLI fallback path; see rtwd.h) --------- */
int find_ifname(const uint8_t *mac, char *out, size_t outsz)
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
void iface_ipv4(const char *ifname, char *out, size_t outsz)
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

/* netcfg helper: ABSOLUTE paths only — rtwd runs as root, and resolving a
 * relative path from an attacker-writable cwd would execute their script. */
static const char *find_netcfg(void)
{
    static const char *cands[] = {
        "/usr/local/libexec/rtw88-eth-netcfg.sh",
        "/usr/local/bin/rtw88-eth-netcfg.sh", NULL };
    for (int i = 0; cands[i]; i++) if (access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

/* find the card's enX (by MAC) and hand it to configd for DHCP. */
void data_online(const uint8_t *mac, char *ifname_out, size_t outsz)
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
/* send a full buffer to a client fd (SO_SNDTIMEO bounds a stuck client). */
static void send_all(int fd, const char *s)
{
    size_t n = strlen(s), off = 0;
    while (off < n) { ssize_t w = send(fd, s + off, n - off, 0); if (w <= 0) break; off += (size_t)w; }
}

/* saved-network lookup with the password copied out under the cfg lock, so the
 * caller never holds a pointer into g_cfg.nets that forget/connect can shift. */
static int saved_password(const char *ssid, char *pass_out, size_t cap)
{
    int found = 0;
    cfg_lock();
    const struct wifi_network *kn = network_match(ssid);
    if (kn) { strlcpy(pass_out, kn->password, cap); found = 1; }
    cfg_unlock();
    return found;
}

/* serialize one scan entry as a {"network":{...}} line into buf. */
static void net_line(char *buf, size_t cap, const struct rtw_scan_entry *e)
{
    size_t len = 0;
    cfg_lock();
    int saved = network_match(e->ssid) != NULL;
    cfg_unlock();
    appendf(buf, cap, &len, "{\"network\":{\"ssid\":");
    json_str(buf, cap, &len, e->ssid);
    appendf(buf, cap, &len,
            ",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"channel\":%u,\"band\":\"%s\","
            "\"beacons\":%u,\"secure\":%s,\"saved\":%s}}\n",
            e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
            e->channel, e->channel <= 14 ? "2.4" : "5", e->beacons,
            e->privacy ? "true" : "false", saved ? "true" : "false");
}

/* Streaming scan: scan the channel list a few channels at a time and emit each
 * newly-seen network as its own JSON line as soon as its chunk completes, then a
 * final {"done":true}. The GUI appends rows live instead of waiting ~11s. */
static void cmd_scan(int fd)
{
    static const unsigned char chans5[] = {
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140, 149, 153, 157, 161, 165 };
    static const unsigned char chans24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };

    /* honor the configured band (band = 2.4 | 5 | both in rtw88.conf) */
    unsigned char chans[40]; int total = 0;
    cfg_lock();
    int band = g_cfg.band;
    cfg_unlock();
    if (band != BAND_24) { memcpy(chans + total, chans5, sizeof chans5); total += (int)sizeof chans5; }
    if (band != BAND_5)  { memcpy(chans + total, chans24, sizeof chans24); total += (int)sizeof chans24; }
    const int CHUNK = 6;

    if (g_radio_off) { send_all(fd, "{\"ok\":false,\"error\":\"radio is off\"}\n"); return; }

    kext_lock();
    if (rtw_hw_open() != 0) { kext_unlock(); send_all(fd, "{\"ok\":false,\"error\":\"kext not reachable\"}\n"); return; }
    send_all(fd, "{\"ok\":true,\"scanning\":true}\n");

    uint8_t seen[128][6]; int nseen = 0;
    static struct rtw_scan_result sr;
    int begun = 0, ended = 0;
    for (int off = 0; off < total; off += CHUNK) {
        struct rtw_scan_chans in;
        memset(&in, 0, sizeof in);
        int last = (off + CHUNK >= total);
        in.flags = (off == 0 ? RTW_SCAN_F_BEGIN : 0) | (last ? RTW_SCAN_F_END : 0);
        in.count = (off + CHUNK <= total) ? (uint32_t)CHUNK : (uint32_t)(total - off);
        memcpy(in.ch, chans + off, in.count);

        if (hw_kctl_scan_chunk(&in, &sr) < 0) continue;
        begun = 1;
        if (last) ended = 1;
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
    /* If the END-carrying chunk RPC failed, the kext is stranded powered-up with
     * bus-mastering on (g_scan_active=1) until something else tears it down — send
     * an explicit END so the device is always quiesced when the scan returns. */
    if (begun && !ended) {
        struct rtw_scan_chans end;
        memset(&end, 0, sizeof end);
        end.flags = RTW_SCAN_F_END;
        hw_kctl_scan_chunk(&end, &sr);
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

static void cmd_connect(const char *ssid, const char *pass, int nosave, char *out, size_t cap)
{
    if (!ssid || !ssid[0]) { snprintf(out, cap, "{\"ok\":false,\"error\":\"missing ssid\"}\n"); return; }
    if (g_radio_off)       { snprintf(out, cap, "{\"ok\":false,\"error\":\"radio is off\"}\n"); return; }

    /* no password given? fall back to a saved one for this SSID (known network). */
    char saved[64];
    const char *use_pass = (pass && pass[0]) ? pass : NULL;
    if (!use_pass && saved_password(ssid, saved, sizeof saved)) use_pass = saved;

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
     * unless the client asked not to. (Only WPA2 networks reach here: the in-kext
     * data path is CCMP-only, so an open-network join never starts the data path
     * and never gets this far — see KNOWN_ISSUES.) DHCP runs outside the kext lock. */
    if (!nosave && use_pass && use_pass[0]) {
        cfg_lock();
        config_set_network(ssid, use_pass);
        config_save(RTWD_CONF_PATH);
        cfg_unlock();
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
    cfg_lock();
    int found = (config_forget_network(ssid) == 0);
    if (found) config_save(RTWD_CONF_PATH);
    cfg_unlock();
    snprintf(out, cap, "{\"ok\":%s}\n", found ? "true" : "false");
}

/* power the radio on/off (a Wi-Fi master switch, separate from connect). OFF
 * tears down any connection + halts the chip (trx_free disables bus-mastering)
 * and pauses auto-connect; ON re-arms auto-connect (which rejoins a known net).
 * The state survives a daemon restart via a flag file. */
static void cmd_power(const char *arg, char *out, size_t cap)
{
    int on  = arg && (!strcasecmp(arg, "on")  || !strcmp(arg, "1") || !strcasecmp(arg, "true"));
    int off = arg && (!strcasecmp(arg, "off") || !strcmp(arg, "0") || !strcasecmp(arg, "false"));
    /* require an explicit on|off: don't let a typo (or a bare `power`) silently power
     * the radio OFF — which now persists across reboots via the flag file. */
    if (!on && !off) { snprintf(out, cap, "{\"ok\":false,\"error\":\"expected on|off\"}\n"); return; }
    if (on) {
        g_radio_off = 0;
        g_user_off  = 0;   /* let auto-connect bring a known network back */
        unlink(RTWD_RADIO_OFF_FLAG);
    } else {
        g_radio_off = 1;
        g_user_off  = 1;
        int fd = open(RTWD_RADIO_OFF_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
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
        cfg_lock(); int have_nets = g_cfg.n_nets; cfg_unlock();
        if (g_user_off || g_radio_off || have_nets == 0) { fails = 0; continue; }

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

        /* copy the chosen ssid+password OUT under the lock: a concurrent
         * forget/connect can shift g_cfg.nets under a held pointer */
        char ssid[33] = "", pass[64] = "";
        unsigned best_beacons = 0; int found = 0;
        cfg_lock();
        for (unsigned k = 0; k < sr.count; k++) {
            const struct rtw_scan_entry *e = &sr.nets[k];
            const struct wifi_network *kn = network_match(e->ssid);
            if (kn && e->beacons >= best_beacons) {
                best_beacons = e->beacons;
                strlcpy(ssid, kn->ssid, sizeof ssid);
                strlcpy(pass, kn->password, sizeof pass);
                found = 1;
            }
        }
        cfg_unlock();
        if (!found || g_user_off) continue;   /* re-check g_user_off: user may have acted during the scan */

        struct rtw_connect_result res;
        int rc = kconnect(ssid, pass, &res);
        if (rc == 0 && ((res.status >> 3) & 1)) {
            char ifname[IFNAMSIZ] = "";
            data_online(res.mac, ifname, sizeof ifname);
            fprintf(stderr, "rtwd: auto-connected to %s (%s)\n", ssid, ifname);
            fails = 0;
        } else {
            /* back off a network that keeps failing (e.g. wrong saved password) so we
             * don't churn the chip's power-on/DMA-ring setup every 15s: 30s, 60s, 120s. */
            if (fails < 8) fails++;
            int mult = fails < 3 ? (1 << fails) : 8;     /* 2,4,8,8,... */
            delay = 15 * mult; if (delay > 120) delay = 120;
            fprintf(stderr, "rtwd: auto-connect to %s failed (status 0x%x); retry in %ds\n",
                    ssid, res.status, delay);
        }
    }
    return NULL;
}

/* ---- access control + line reading ---------------------------------------- */

/* allow root and the console (logged-in GUI) user only: the socket is the
 * control plane of a root daemon, so gate by peer credential instead of relying
 * on file modes alone. */
static int peer_allowed(int fd)
{
    struct xucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) != 0 ||
        cred.cr_version != XUCRED_VERSION)
        return 0;
    if (cred.cr_uid == 0) return 1;
    struct stat console;
    if (stat("/dev/console", &console) == 0 && cred.cr_uid == console.st_uid) return 1;
    return 0;
}

/* read one command line (until '\n'), looping over partial reads. The client fd
 * carries SO_RCVTIMEO, so a stalled client cannot wedge the daemon. Returns the
 * length on a complete line, -1 if it timed out / disconnected without a newline,
 * or -2 if the line overflowed the buffer (so the caller rejects it instead of
 * executing a truncated command). */
static ssize_t read_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    int got_nl = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(buf, '\n', n)) { got_nl = 1; break; }
    }
    buf[n] = '\0';
    if (got_nl) return (ssize_t)n;
    if (n + 1 >= cap) return -2;   /* full buffer, no terminator -> too long */
    return n ? -1 : -1;            /* EOF/timeout mid-line -> treat as incomplete */
}

/* a TAB/newline inside an argument would shift the protocol fields (an SSID is
 * attacker-named over the air) — already split, but reject embedded controls. */
static int arg_clean(const char *s)
{
    if (!s) return 1;
    for (; *s; s++) if ((unsigned char)*s < 0x20) return 0;
    return 1;
}

/* ---- one client connection: read a command line, reply, close ------------ */
static void handle_client(int fd)
{
    /* bound both directions: a dead/stuck client may not consume our reply, and
     * send() while we hold the kext lock must never block forever. */
    struct timeval tmo = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);

    if (!peer_allowed(fd)) {
        send_all(fd, "{\"ok\":false,\"error\":\"not authorized\"}\n");
        return;
    }

    char req[256];
    ssize_t rl = read_line(fd, req, sizeof req);
    if (rl == -2) { send_all(fd, "{\"ok\":false,\"error\":\"line too long\"}\n"); return; }
    if (rl <= 0) return;   /* incomplete / disconnected */
    char *nl = strpbrk(req, "\r\n"); if (nl) *nl = '\0';

    /* split on TAB: verb [ssid] [pass] [flag] */
    char *verb = req, *a1 = NULL, *a2 = NULL, *a3 = NULL;
    char *t = strchr(req, '\t');
    if (t) {
        *t = '\0'; a1 = t + 1;
        char *t2 = strchr(a1, '\t');
        if (t2) {
            *t2 = '\0'; a2 = t2 + 1;
            char *t3 = strchr(a2, '\t');
            if (t3) { *t3 = '\0'; a3 = t3 + 1; }
        }
    }
    if (!arg_clean(a1) || !arg_clean(a2) || !arg_clean(a3)) {
        send_all(fd, "{\"ok\":false,\"error\":\"bad argument\"}\n");
        return;
    }

    /* scan streams multiple JSON lines straight to the socket as chunks complete. */
    if (!strcmp(verb, "scan")) { cmd_scan(fd); return; }

    static char out[4096];
    out[0] = '\0';
    if      (!strcmp(verb, "connect"))    cmd_connect(a1, a2, a3 && !strcmp(a3, "nosave"), out, sizeof out);
    else if (!strcmp(verb, "disconnect")) cmd_disconnect(out, sizeof out);
    else if (!strcmp(verb, "status"))     cmd_status(out, sizeof out);
    else if (!strcmp(verb, "forget"))     cmd_forget(a1, out, sizeof out);
    else if (!strcmp(verb, "power"))      cmd_power(a1, out, sizeof out);
    else snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"unknown command\"}\n");

    send_all(fd, out);
}

int rtwd_daemon_main(void)
{
    signal(SIGPIPE, SIG_IGN);

    /* single-instance guard: hold an exclusive flock on a lock file for the daemon's
     * lifetime. Atomic (no connect-probe TOCTOU): a second rtwd's non-blocking flock
     * fails immediately, so it exits instead of unlink()ing a live daemon's socket and
     * running a competing auto-connect loop. The lock is released automatically when the
     * process exits (so a crashed daemon's lock is never stale). */
    static int lock_fd = -1;
    lock_fd = open(RTWD_LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "rtwd: another instance is already running (lock %s held)\n", RTWD_LOCK_PATH);
        return 1;
    }

    unlink(RTWD_SOCK_PATH);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strlcpy(sa.sun_path, RTWD_SOCK_PATH, sizeof sa.sun_path);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); return 1; }
    /* world-connectable mode, but handle_client gates by peer credential
     * (root or the console user) — see peer_allowed(). */
    chmod(RTWD_SOCK_PATH, 0666);
    if (listen(s, 8) != 0) { perror("listen"); return 1; }

    /* load saved networks + the persisted radio switch, then auto-connect to the
     * strongest known network on boot. */
    config_load_file(RTWD_CONF_PATH);
    g_radio_off = (access(RTWD_RADIO_OFF_FLAG, F_OK) == 0);
    if (g_radio_off) g_user_off = 1;
    pthread_t auto_t;
    if (pthread_create(&auto_t, NULL, auto_connect_thread, NULL) == 0)
        pthread_detach(auto_t);

    fprintf(stderr, "rtwd: listening on %s (%d known network(s)%s)\n",
            RTWD_SOCK_PATH, g_cfg.n_nets, g_radio_off ? ", radio off" : "");
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
