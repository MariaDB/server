#!/bin/sh
# Run csmith: c2m interpreter vs gcc:
#

trap_ctrlc() {
  echo finished
  rm -f c2m.out gcc.out gcc-a.out tttt.c tttt.bmir
  exit 0
}

trap trap_ctrlc INT

if type timeout >/dev/null 2>&1;then
    TIMEOUT="timeout 10s"
elif type gtimeout >/dev/null 2>&1;then
    TIMEOUT="gtimeout 10s"
else
    TIMEOUT=
fi
i=0;
while test $i -lt 10000;do
  ${CSMITH_HOME}/bin/csmith --no-packed-struct > tttt.c #  --no-bitfields
  if ./c2m -I${CSMITH_HOME}/include -w tttt.c -o tttt.bmir;then echo -n .;else echo c2m failed; exit 1;fi
  if gcc -I${CSMITH_HOME}/include -w tttt.c -o gcc-a.out;then echo -n +;else echo gcc failed; exit 1;fi
  $TIMEOUT ./gcc-a.out >gcc.out; res=$?
  $TIMEOUT sh -c './c2m tttt.bmir -ei >c2m.out'; c2mres=$?
  if test $res -gt 127 || test $c2mres -gt 127; then
# a signal (seg fault, bus error etc)
      if test $res -ne $c2mres; then exit 1; fi
  fi
# exit code 124 is timeout
  if test $res -eq 0 && test $c2mres -ne 0 && test $c2mres -ne 124; then echo only c2m JIT failed; exit 1; fi
  if test $c2mres -eq 0 && test $res -ne 0 && test $res -ne 124; then echo only gcc code failed; exit 1; fi
  if test $res -eq 0 && test $c2mres -ne 0; then echo c2m code only timeout; fi
  if test $c2mres -eq 0 && test $res -ne 0; then echo gcc code only timeout; fi
  if test $c2mres -eq 0 && test $res -eq 0; then
    if cmp c2m.out gcc.out;then echo -n =;else diff -up c2m.out gcc.out; wc tttt.c; exit 1;fi
  else
    echo -n "?"
  fi
  i=`expr $i + 1`
  if expr $i % 25 = 0 >/dev/null; then echo;fi
done

trap_ctrlc
