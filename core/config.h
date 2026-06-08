/*
 * config.h — runtime configuration for RTW88Client.
 *
 * Nothing network-specific is hardcoded. A config file and/or CLI flags supply
 * one or MORE known networks (SSID + passphrase); at run time the client scans
 * and joins the strongest network that matches one it has credentials for — so
 * the same binary + config roams across home/office/phone-hotspot.
 */
#ifndef RTW88_CONFIG_H
#define RTW88_CONFIG_H

enum { BAND_BOTH = 0, BAND_24 = 1, BAND_5 = 2 };
enum { MODE_SCAN = 0,   /* scan + list networks only (no association)         */
       MODE_ETH  = 1 }; /* join + present a macOS Ethernet interface (enX) via the kext */

#define RTW88_MAX_NETWORKS 16

struct wifi_network {
    char ssid[33];      /* network name                                       */
    char password[64];  /* WPA2-PSK passphrase ("" = open network)            */
};

struct wifi_config {
    struct wifi_network nets[RTW88_MAX_NETWORKS];
    int  n_nets;        /* number of known networks                           */
    int  band;          /* BAND_BOTH | BAND_24 | BAND_5                       */
    int  mode;          /* MODE_SCAN | MODE_ETH                                */
    /* optional static-IP overrides (0 = use DHCP-learned value)              */
    unsigned ip, netmask, gateway, dns;
};

extern struct wifi_config g_cfg;

/* Negotiated link state shared across the join + data paths. The AP's HT/VHT
 * capability (learned from the association response) decides the rate-adaptation
 * "raid" we program into the firmware and stamp into every data TX descriptor, so
 * the same binary adapts: VHT/11ac on a 5GHz ac AP, HT/11n on an n AP, else legacy.
 * Set during association (caps) + media-connect (raid); read by the TX data paths. */
struct wifi_session {
    unsigned char has_ht;     /* AP advertised an HT Capabilities (45) element    */
    unsigned char has_vht;    /* AP advertised a VHT Capabilities (191) element   */
    unsigned char raid;       /* RTW_RATEID_* chosen for our TX rate table        */
    unsigned char init_rate;  /* DESC_RATE* initial hint placed in TX descriptors */
};
extern struct wifi_session g_session;

/* Parse a config file then CLI flags (CLI wins). Returns 0 to proceed, 1 to
 * exit cleanly (e.g. --help), -1 on a usage error. */
int parse_args(int argc, char **argv);

/* If `ssid` is one of the configured networks, return its entry (with the
 * passphrase); else NULL. Used to pick a join target from the scan results. */
const struct wifi_network *network_match(const char *ssid);

/* Runtime config persistence (rtwd remembers networks the GUI connects to). */
int config_load_file(const char *path);                       /* populate g_cfg from file (missing = empty) */
int config_set_network(const char *ssid, const char *pass);   /* add or update a saved network */
int config_forget_network(const char *ssid);                  /* remove a saved network (0 = removed) */
int config_save(const char *path);                            /* rewrite the file from g_cfg (0600) */

#endif /* RTW88_CONFIG_H */
