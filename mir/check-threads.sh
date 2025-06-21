#!/bin/sh
# Check pthread library presence:
#

echo "#include <pthread.h>" >__tmp.c && \
  echo "void *f (void *a) {} void main (void) {pthread_t t1;pthread_create (&t1, NULL, &f, NULL);}" >>__tmp.c && \
  cc -w __tmp.c -lpthread -o __tmp.out
ok=$?
rm -f __tmp.c __tmp.out
if test $ok; then echo ok; else echo fail; fi
exit 0

