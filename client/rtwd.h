/*
 * rtwd.h — shared between the daemon personality (rtwd.c) and the CLI
 * personality (main.c) of the single `rtwd` binary.
 */
#ifndef RTWD_H
#define RTWD_H

#include <stddef.h>
#include <stdint.h>

#define RTWD_SOCK_PATH       "/var/run/rtw88d.sock"
#define RTWD_LOCK_PATH       "/var/run/rtw88d.lock"
#define RTWD_CONF_PATH       "/usr/local/etc/rtw88.conf"
#define RTWD_RADIO_OFF_FLAG  "/var/db/rtw88.radio-off"

/* daemon entry point (rtwd with no arguments) */
int rtwd_daemon_main(void);

/* enX bring-up helpers (one implementation, used by daemon + CLI fallback) */
int  find_ifname(const uint8_t *mac, char *out, size_t outsz);
void iface_ipv4(const char *ifname, char *out, size_t outsz);
void data_online(const uint8_t *mac, char *ifname_out, size_t outsz);

#endif /* RTWD_H */
