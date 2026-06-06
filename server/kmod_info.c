/*
 * kmod_info.c — _kmod_info + module start/stop glue required of every kext by
 * the kernel-collection linker (kmutil). Mirrors probe/kmod_info.c.
 *
 * NOTE: this kext uses its OWN bundle id (com.rtw88.RTW88Server), distinct
 * from the driver/probe (com.rtw88.RTL8821CEwifi), because it is a separate
 * development vehicle. It needs ONE System Settings approval the first time it
 * loads; after that its cdhash never changes (you only edit userspace), so it
 * never prompts again.
 */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) kern_return_t
RTW88Server_start(kmod_info_t *ki, void *d) { (void)ki; (void)d; return KERN_SUCCESS; }
__attribute__((visibility("default"))) kern_return_t
RTW88Server_stop(kmod_info_t *ki, void *d) { (void)ki; (void)d; return KERN_SUCCESS; }

KMOD_EXPLICIT_DECL(com.rtw88.RTW88Server, "1.0.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = RTW88Server_start;
__private_extern__ kmod_stop_func_t  *_antimain = RTW88Server_stop;
