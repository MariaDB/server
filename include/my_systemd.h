
#ifndef MY_SYSTEMD_INCLUDED
#define MY_SYSTEMD_INCLUDED

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>

#else



#define sd_notify(X, Y)
#define sd_notifyf(E, F, ...)

#endif

#endif /* MY_SYSTEMD_INCLUDED */
