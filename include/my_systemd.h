
#ifndef MY_SYSTEMD_INCLUDED
#define MY_SYSTEMD_INCLUDED

#if defined(HAVE_SYSTEMD) && !defined(EMBEDDED_LIBRARY)
#include <systemd/sd-daemon.h>

#else



#define sd_notify(X, Y)
#define sd_notifyf(E, F, ...)

#endif

#endif /* MY_SYSTEMD_INCLUDED */
