/*
 * config.c — CLI + config-file parsing for RTW88Client.
 *
 *   RTW88Client [options]
 *     --config <file>    read settings from a file first (CLI flags override)
 *     --ssid <name>      add a known network (repeatable)
 *     --password <pass>  passphrase for the most recent --ssid (omit = open)
 *     --band 2.4|5|both  band(s) to scan                       (default both)
 *     --mode eth|scan    eth=join + macOS enX (default if a network
 *                        is configured), scan=list networks only
 *     --ip/--netmask/--gateway/--dns   static-IP overrides (optional; else DHCP)
 *     --help
 *
 * Config file: `key = value` per line, '#' starts a comment. Multiple networks:
 * each `network = <ssid>` (or `ssid = <ssid>`) starts an entry and the following
 * `password = <pass>` applies to it — list as many as you like:
 *
 *     band = 5
 *     mode = eth
 *     network = HomeWiFi
 *     password = homesecret
 *     network = Phone Hotspot
 *     password = hotspotpass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
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

static int load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "config: cannot open %s\n", path); return -1; }
    char line[512]; int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char key[64], val[256];
        strncpy(key, line, sizeof key - 1); key[sizeof key-1]=0; trim(key);
        strncpy(val, eq + 1, sizeof val - 1); val[sizeof val-1]=0; trim(val);
        if (!key[0]) continue;
        if (apply(key, val) != 0)
            fprintf(stderr, "config: %s:%d: bad setting '%s = %s'\n", path, lineno, key, val);
    }
    fclose(f);
    return 0;
}

static void usage(const char *argv0)
{
    printf(
"usage: %s [options]\n"
"  --config <file>      load settings from a file (CLI flags override)\n"
"  --ssid <name>        add a known network (repeatable)\n"
"  --password <pass>    passphrase for the most recent --ssid (omit = open)\n"
"  --band 2.4|5|both    band(s) to scan                    (default both)\n"
"  --mode eth|scan      eth=join + macOS Ethernet enX        (default eth if a\n"
"                       scan=list networks only             network is set)\n"
"  --ip/--netmask/--gateway/--dns  static-IP overrides (optional; else DHCP)\n"
"  --help\n"
"\nexamples:\n"
"  sudo %s --ssid MyWiFi --password secret --band 5 --mode eth\n"
"  sudo %s --ssid Home --password h --ssid Phone --password p   # roams\n"
"  sudo %s --config /usr/local/etc/rtw88.conf\n"
"  %s --mode scan        # just list visible networks (no sudo)\n",
        argv0, argv0, argv0, argv0, argv0);
}

int parse_args(int argc, char **argv)
{
    /* a config file given with --config is read first so flags can override it */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config")) { if (load_file(argv[i+1]) != 0) return -1; }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr,"%s needs a value\n",a), (const char*)NULL))
        if (!strcmp(a,"--help")||!strcmp(a,"-h")) { usage(argv[0]); return 1; }
        else if (!strcmp(a,"--config")) { i++; /* already handled */ }
        else if (!strcmp(a,"--ssid")||!strcmp(a,"--network")) { const char*v=NEXT(); if(!v)return -1; apply("ssid",v); }
        else if (!strcmp(a,"--password")||!strcmp(a,"--pass")) { const char*v=NEXT(); if(!v)return -1; if(apply("password",v)){return -1;} }
        else if (!strcmp(a,"--band"))     { const char*v=NEXT(); if(!v)return -1; if(apply("band",v)){fprintf(stderr,"bad --band %s\n",v);return -1;} }
        else if (!strcmp(a,"--mode"))     { const char*v=NEXT(); if(!v)return -1; if(apply("mode",v)){fprintf(stderr,"bad --mode %s\n",v);return -1;} }
        else if (!strcmp(a,"--ip"))       { const char*v=NEXT(); if(!v)return -1; apply("ip",v); }
        else if (!strcmp(a,"--netmask"))  { const char*v=NEXT(); if(!v)return -1; apply("netmask",v); }
        else if (!strcmp(a,"--gateway")||!strcmp(a,"--gw")) { const char*v=NEXT(); if(!v)return -1; apply("gateway",v); }
        else if (!strcmp(a,"--dns"))      { const char*v=NEXT(); if(!v)return -1; apply("dns",v); }
        else if (parse_mode(a) >= 0)      { g_cfg.mode = parse_mode(a); }  /* bare token: eth|scan */
        else { fprintf(stderr, "unknown option: %s (try --help)\n", a); return -1; }
        #undef NEXT
    }

    /* default mode: eth (full system connectivity via enX) if we have a network, else scan */
    if (g_cfg.mode < 0) g_cfg.mode = g_cfg.n_nets ? MODE_ETH : MODE_SCAN;

    if (g_cfg.mode != MODE_SCAN && g_cfg.n_nets == 0) {
        fprintf(stderr, "error: at least one --ssid (or 'network =' in the config) "
                        "is required for --mode eth\n");
        return -1;
    }
    return 0;
}
