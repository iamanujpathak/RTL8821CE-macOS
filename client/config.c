/*
 * config.c — rtw88.conf parsing + persistence for the rtwd daemon.
 *
 * Config file: `key = value` per line; a line whose first non-blank character is
 * '#' is a comment. '#' is NOT a comment inside a value, so a WPA2 passphrase may
 * contain '#' and interior spaces and round-trips through save/load unchanged.
 * (LEADING/TRAILING whitespace in a value is trimmed — a passphrase that begins or
 * ends with a space is not preserved; such passphrases are pathological and the GUI
 * cannot enter them either.)
 * Multiple networks: each `network = <ssid>` (or `ssid = <ssid>`) starts an entry
 * and the following `password = <pass>` applies to it — list as many as you like:
 *
 *     band = 5
 *     network = HomeWiFi
 *     password = homesecret
 *     network = Phone Hotspot
 *     password = hotspotpass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include "config.h"

struct wifi_config g_cfg = { .band = BAND_BOTH, .mode = -1 };

const struct wifi_network *network_match(const char *ssid)
{
    for (int i = 0; i < g_cfg.n_nets; i++)
        if (strcmp(g_cfg.nets[i].ssid, ssid) == 0) return &g_cfg.nets[i];
    return NULL;
}

/* start a new network entry with this SSID (or return the existing one) */
static struct wifi_network *add_network(const char *ssid)
{
    for (int i = 0; i < g_cfg.n_nets; i++)
        if (strcmp(g_cfg.nets[i].ssid, ssid) == 0) return &g_cfg.nets[i];
    if (g_cfg.n_nets >= RTW88_MAX_NETWORKS) {
        fprintf(stderr, "config: too many networks (max %d)\n", RTW88_MAX_NETWORKS);
        return NULL;
    }
    struct wifi_network *n = &g_cfg.nets[g_cfg.n_nets++];
    memset(n, 0, sizeof *n);
    strncpy(n->ssid, ssid, sizeof n->ssid - 1);
    return n;
}

static void trim(char *s)
{
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n' || s[n-1]=='\r' || s[n-1]==' ' || s[n-1]=='\t')) s[--n] = 0;
}

/* strip a WHOLE-LINE comment only: a '#' as the first non-blank character makes the
 * line a comment. We deliberately do NOT honor inline trailing '#' comments, so a
 * value (SSID or WPA2 passphrase) may contain '#' and any spaces and still round-trips
 * through config_save/load unchanged. */
static void strip_comment(char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') line[0] = 0;
}

static int parse_band(const char *v)
{
    if (!strcmp(v,"2.4")||!strcmp(v,"2")||!strcmp(v,"2g")||!strcmp(v,"2.4g")) return BAND_24;
    if (!strcmp(v,"5")||!strcmp(v,"5g")) return BAND_5;
    if (!strcmp(v,"both")||!strcmp(v,"all")) return BAND_BOTH;
    return -1;
}
static int parse_mode(const char *v)
{
    if (!strcmp(v,"eth")||!strcmp(v,"ethernet")) return MODE_ETH;
    if (!strcmp(v,"scan")||!strcmp(v,"none")) return MODE_SCAN;
    return -1;
}
static unsigned parse_ip(const char *v)
{
    struct in_addr a; if (inet_aton(v, &a) == 0) return 0;
    return ntohl(a.s_addr);
}

/* apply one key=value setting; returns 0 ok, -1 bad value */
static int apply(const char *key, const char *val)
{
    if (!strcmp(key,"ssid") || !strcmp(key,"network"))
                                      { return add_network(val) ? 0 : -1; }
    else if (!strcmp(key,"password")||!strcmp(key,"pass")) {
        if (g_cfg.n_nets == 0) { fprintf(stderr, "config: 'password' before any 'network'/'ssid'\n"); return -1; }
        strncpy(g_cfg.nets[g_cfg.n_nets-1].password, val,
                sizeof g_cfg.nets[0].password - 1);
    }
    else if (!strcmp(key,"band"))     { int b = parse_band(val); if (b<0) return -1; g_cfg.band = b; }
    else if (!strcmp(key,"mode"))     { int m = parse_mode(val); if (m<0) return -1; g_cfg.mode = m; }
    else if (!strcmp(key,"ip"))       { g_cfg.ip = parse_ip(val); }
    else if (!strcmp(key,"netmask"))  { g_cfg.netmask = parse_ip(val); }
    else if (!strcmp(key,"gateway")||!strcmp(key,"gw")) { g_cfg.gateway = parse_ip(val); }
    else if (!strcmp(key,"dns"))      { g_cfg.dns = parse_ip(val); }
    else return -1;
    return 0;
}

/* does this key's value contain a secret? (keeps it out of error logs) */
static int key_is_secret(const char *key)
{
    return strcasestr(key, "pass") != NULL;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "config: cannot open %s\n", path); return -1; }
    char line[512]; int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        strip_comment(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char key[64], val[256];
        strncpy(key, line, sizeof key - 1); key[sizeof key-1]=0; trim(key);
        strncpy(val, eq + 1, sizeof val - 1); val[sizeof val-1]=0; trim(val);
        if (!key[0]) continue;
        if (apply(key, val) != 0)
            fprintf(stderr, "config: %s:%d: bad setting '%s = %s'\n", path, lineno, key,
                    key_is_secret(key) ? "********" : val);
    }
    fclose(f);
    return 0;
}

/* ---- runtime config persistence (rtwd remembers networks) ------------------ */

/* Public load entry. Missing file is fine — start with an empty known-network
 * list. Returns 0 always. */
int config_load_file(const char *path)
{
    load_file(path);   /* logs + returns -1 if absent; we treat that as "no saved nets" */
    return 0;
}

/* Add a network, or update its password if already known. Returns 0 on success. */
int config_set_network(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return -1;
    struct wifi_network *n = add_network(ssid);
    if (!n) return -1;
    n->password[0] = 0;
    if (pass) { strncpy(n->password, pass, sizeof n->password - 1); n->password[sizeof n->password - 1] = 0; }
    return 0;
}

/* Remove a saved network by SSID. Returns 0 if removed, -1 if not found. */
int config_forget_network(const char *ssid)
{
    for (int i = 0; i < g_cfg.n_nets; i++) {
        if (strcmp(g_cfg.nets[i].ssid, ssid) == 0) {
            for (int j = i + 1; j < g_cfg.n_nets; j++) g_cfg.nets[j - 1] = g_cfg.nets[j];
            g_cfg.n_nets--;
            return 0;
        }
    }
    return -1;
}

static const char *band_str(int b)
{ return b == BAND_24 ? "2.4" : b == BAND_5 ? "5" : "both"; }

static void ip_str(unsigned v, char *out, size_t cap)
{ struct in_addr a; a.s_addr = htonl(v); strncpy(out, inet_ntoa(a), cap - 1); out[cap - 1] = 0; }

/* Rewrite the config file from g_cfg (networks + band/static IP). Written to a
 * temp file created 0600 (it holds passphrases — never visible with looser
 * modes, even briefly) and renamed into place so a crash mid-write can't leave
 * a truncated config. Loses the example file's comments; idempotent. */
int config_save(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { fprintf(stderr, "config: cannot write %s\n", tmp); return -1; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    fprintf(f, "# rtw88.conf — written by rtwd. `key = value`; '#' starts a comment.\n");
    fprintf(f, "# Known networks (each 'network =' starts an entry; 'password =' is its key).\n\n");
    for (int i = 0; i < g_cfg.n_nets; i++) {
        fprintf(f, "network  = %s\n", g_cfg.nets[i].ssid);
        if (g_cfg.nets[i].password[0])
            fprintf(f, "password = %s\n", g_cfg.nets[i].password);
        fprintf(f, "\n");
    }
    fprintf(f, "band = %s\n", band_str(g_cfg.band));
    if (g_cfg.ip || g_cfg.netmask || g_cfg.gateway || g_cfg.dns) {
        char b[16];
        if (g_cfg.ip)      { ip_str(g_cfg.ip, b, sizeof b);      fprintf(f, "ip      = %s\n", b); }
        if (g_cfg.netmask) { ip_str(g_cfg.netmask, b, sizeof b); fprintf(f, "netmask = %s\n", b); }
        if (g_cfg.gateway) { ip_str(g_cfg.gateway, b, sizeof b); fprintf(f, "gateway = %s\n", b); }
        if (g_cfg.dns)     { ip_str(g_cfg.dns, b, sizeof b);     fprintf(f, "dns     = %s\n", b); }
    }
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "config: cannot replace %s\n", path);
        unlink(tmp);
        return -1;
    }
    return 0;
}
