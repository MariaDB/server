#!/bin/sh

tempc=c-benchmarks/__temp.c
tempwasm=c-benchmarks/__ogg.wasm
tempogg=c-benchmarks/__jfk.ogg
tempexec=c-benchmarks/__oggenc

cat >$tempc <<EOF
  #include <stdio.h>
  int main (void) {printf ("hi\n"); return 0;}
EOF

  echo -n "gcc -O0:		" && gcc -O0 c-benchmarks/oggenc.c -w -o $tempexec -lm && $tempexec - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
echo -n "gcc -O2:		" && gcc -O2 c-benchmarks/oggenc.c -w -o $tempexec -lm && $tempexec - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
echo -n "c2m -O0:		" && ./c2m -O0 c-benchmarks/oggenc.c -w -eg - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
echo -n "c2m -O1:		" && ./c2m -O1 c-benchmarks/oggenc.c -w -eg - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
echo -n "c2m -O2:		" && ./c2m -O2 c-benchmarks/oggenc.c -w -eg - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
echo -n "c2m -O3:		" && ./c2m -O3 c-benchmarks/oggenc.c -w -eg - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
if wasi-clang $tempc >/dev/null 2>&1 && wasmer run ./a.out >/dev/null 2>&1; then
  echo -n "wasi -O0 && wasmer:	" && wasi-clang -O0 c-benchmarks/oggenc.c -w -o $tempwasm && wasmer $tempwasm -- - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
  echo -n "wasi -O2 && wasmer:	" && wasi-clang -O2 c-benchmarks/oggenc.c -w -o $tempwasm && wasmer $tempwasm -- - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
  echo -n "wasi -O2 && wasmer LLVM:" && wasi-clang -O2 c-benchmarks/oggenc.c -w -o $tempwasm && wasmer run --llvm $tempwasm -- - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
fi

if wasi-clang $tempc >/dev/null 2>&1 && wasmtime ./a.out >/dev/null 2>&1; then
  echo -n "wasi -O0 && wasmtime:	" && wasi-clang -O0 c-benchmarks/oggenc.c -w -o $tempwasm && wasmtime $tempwasm -- - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
  echo -n "wasi -O2 && wasmtime:	" && wasi-clang -O2 c-benchmarks/oggenc.c -w -o $tempwasm && wasmtime $tempwasm -- - <c-benchmarks/jfk_1963_0626_berliner.wav 2>&1 >$tempogg | grep -F Rate
fi

rm -f $tempc $tempexec $tempwasm $tempogg ./a.out
