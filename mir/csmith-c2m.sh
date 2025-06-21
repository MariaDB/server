#!/bin/bash
# Run csmith: c2m interpreter vs JIT:
#

trap_ctrlc() {
  echo finished
  rm -f c2m-interp.out c2m-jit.out ttt.c ttt.bmir
  exit 0
}

trap trap_ctrlc INT

if type timeout >/dev/null 2>&1;then
    TIMEOUT="timeout 15s"
elif type gtimeout >/dev/null 2>&1;then
    TIMEOUT="gtimeout 15s"
else
    TIMEOUT=
fi
i=0;
while test $i -lt 10000;do
  ${CSMITH_HOME}/bin/csmith --no-packed-struct > ttt.c #  --no-bitfields
  if ./c2m -I${CSMITH_HOME}/include -w ttt.c -o ttt.bmir;then echo -n .;else echo c2m failed; exit 1;fi
  $TIMEOUT sh -c './c2m ttt.bmir -ei >c2m-interp.out'; c2mires=$?
  $TIMEOUT sh -c './c2m ttt.bmir -el >c2m-jit.out'; c2mjres=$?
  if test $c2mires -gt 127 || test $c2mjres -gt 127; then
     if test $c2mires -ne $c2mjres; then exit 1; fi
  fi
# exit code 124 is timeout
  if test $c2mires -eq 0 && test $c2mjres -ne 0 && test $c2mjres -ne 124; then echo only c2m JIT failed; exit 1; fi
  if test $c2mjres -eq 0 && test $c2mires -ne 0 && test $c2mires -ne 124; then echo only c2m interpeter failed; exit 1; fi
  if test $c2mjres -eq 0 && test $c2mires -eq 0; then
    if cmp c2m-interp.out c2m-jit.out;then echo -n =;else diff -up c2m-interp.out c2m-jit.out; wc ttt.c; exit 1;fi
  else
    echo -n "?"
  fi
  i=`expr $i + 1`
  if expr $i % 25 = 0 >/dev/null; then echo;fi
done

trap_ctrlc
