
#ifndef _my_sdnotify_h
#define _my_sdnotify_h

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>

#else

#define sd_notify(X, Y, Z)
static int sd_notifyf(int unset_environment, const char *format, ...) {}

#endif

#endif /* _my_sdnotify_h */
