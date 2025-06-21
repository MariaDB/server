#!/bin/bash
# Run run-benchmarks.sh [start_test_num] [short]
#

export LC_NUMERIC=

srcdir=`dirname $0`
tempc=__c-bench-temp.c
temp=__c-bench-temp.out
temp2=__c-bench-temp2.out
temp3=__c-bench-temp3.out
errf=__c-bench-temp.err

if test x`echo -n` != "x-n";then NECHO="echo -n"; else NECHO=printf; fi

rm -f $temp3

percent () {
    val=`awk "BEGIN {if ($2==0) print \"Inf\"; else printf \"%.2f\n\", $1/$2;}"`
    echo "$val"x
    echo "$3:$val" >>$temp3
}

skip () {
    l=$1
    n=$2
    while test $l -le $n; do $NECHO " "; l=`expr $l + 1`; done
}

print_time() {
    title="$1"
    secs=$2
    if test "x$NECHO" = x; then
	echo $title:
	echo "   " $secs
    else
	n=$title:
	$NECHO "$n"
	skip ${#n} 40
	$NECHO "$secs"
	skip ${#secs} 10
        echo " " `percent $base_time $secs "$title"`
    fi
}

run () {
  title=$1
  preparation=$2
  program=$3
  expect_out=$4
  inputf=$5
  flag=$6
  ok=
  if test x"$preparation" != x; then
    sh -c "$preparation" 2>$errf
    if test $? != 0; then echo "$2": FAIL; cat $errf; return 1; fi
  fi
  if test x$inputf = x; then inputf=/dev/null;fi
  if (time -p $program < $inputf) >$temp 2>$temp2; then
      ok=y
      if test x$short == xshort; then
        (time -p $program < $inputf) >$temp 2>>$temp2
        (time -p $program < $inputf) >$temp 2>>$temp2
      fi
  fi
  if test x$ok = x;then echo $program: FAILED; return 1; fi
  if test x$expect_out != x && ! cmp $expect_out $temp; then
    echo Unexpected output:
    diff -up $expect_out $temp
    return 1
  fi
  secs=`grep -E 'user[ 	]*[0-9]' $temp2 | sed s/.*user// | sed s/\\t// | sort -n | head -1`
  if test x$flag != x;then base_time=$secs;fi
  print_time "$title" $secs
}

runbench () {
  bench=$1
  arg=$2
  base_time=0.01
  inputf=
  if test -f $bench.expect; then expect_out=$bench.expect; else expect_out=; fi
  cat >$tempc <<EOF
  #include <stdio.h>
  int main (void) {printf ("hi\n"); return 0;}
EOF
  first=first
  if gcc $tempc >/dev/null 2>&1; then
      run "gcc -O2" "gcc -std=c99 -O2 -I$srcdir/c-benchmarks -I. $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      if test x$short != xshort; then
	 run "gcc -O0" "gcc -std=c99 -O0 -I$srcdir/c-benchmarks -I. $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf"
      fi
      first=
  fi
  if test x$short != xshort && clang $tempc >/dev/null 2>&1; then
      run "clang -O2" "clang -std=c99 -O2 -I$srcdir/c-benchmarks -I. $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && pcc $tempc >/dev/null 2>&1; then
      run "pcc -O" "pcc -O $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && cproc $tempc >/dev/null 2>&1; then
      run "cproc" "cproc $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && cparser $tempc >/dev/null 2>&1; then
      run "cparser -O3" "cparser -O3 $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && tcc $tempc >/dev/null 2>&1; then
      run "tcc" "tcc -std=c11 -I$srcdir/c-benchmarks -I. $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && lacc $tempc >/dev/null 2>&1; then
      run "lacc -O3" "lacc -O3 $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && chibicc $tempc >/dev/null 2>&1; then
      run "chibicc" "chibicc $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && ccomp $tempc >/dev/null 2>&1; then
      run "ccomp -O3" "ccomp -O3 $bench.c -lm" "./a.out $arg" "$expect_out" "$inputf" $first
      first=
  fi
  if test x$short != xshort && ! grep -F setjmp.h $bench.c >/dev/null 2>&1; then
    if emcc $tempc -s STANDALONE_WASM >/dev/null 2>&1 && wasmer run ./a.out.wasm >/dev/null 2>&1; then
      run "emcc/wasmer" "emcc -s STANDALONE_WASM -s TOTAL_MEMORY=200mb $bench.c" "wasmer run ./a.out.wasm -- $arg" "$expect_out" "$inputf" $first
    fi
    if emcc $tempc -s STANDALONE_WASM >/dev/null 2>&1 && wasmer run ./a.out.wasm >/dev/null 2>&1; then
      run "emcc -O2/wasmer" "emcc -O2 -s STANDALONE_WASM -s TOTAL_MEMORY=200mb $bench.c" "wasmer run ./a.out.wasm -- $arg" "$expect_out" "$inputf" $first
    fi
    if wasi-clang $tempc >/dev/null 2>&1 && wasmer run --backend=singlepass ./a.out >/dev/null 2>&1; then
      run "wasi -O2/wasmer singlepass" "wasi-clang -O2 $bench.c" "wasmer run --backend=singlepass ./a.out -- $arg" "$expect_out" "$inputf" $first
    fi
    if wasi-clang $tempc >/dev/null 2>&1 && wasmer run --backend=cranelift ./a.out >/dev/null 2>&1; then
      run "wasi -O2/wasmer cranelift" "wasi-clang -O2 $bench.c" "wasmer run --backend=cranelift ./a.out -- $arg" "$expect_out" "$inputf" $first
    fi
    if wasi-clang $tempc >/dev/null 2>&1 && wasmer run --backend=llvm ./a.out >/dev/null 2>&1; then
      run "wasi -O2/wasmer LLVM" "wasi-clang -O2 $bench.c" "wasmer run --backend=llvm ./a.out -- $arg" "$expect_out" "$inputf" $first
    fi
    if wasi-clang $tempc >/dev/null 2>&1 && wasmtime --help >/dev/null 2>&1; then
      run "wasi -O0/wasmtime" "wasi-clang -O0 $bench.c" "wasmtime ./a.out -- $arg" "$expect_out" "$inputf" $first
    fi
    if wasi-clang $tempc >/dev/null 2>&1 && wasmtime --help >/dev/null 2>&1; then
      run "wasi -O2/wasmtime" "wasi-clang -O2 $bench.c" "wasmtime ./a.out -- $arg" "$expect_out" "$inputf" $first
    fi
  fi
  run "c2m -eg" "" "./c2m -I$srcdir/c-benchmarks -I. $bench.c -eg $arg" "$expect_out" "$inputf" $first
  run "c2m -eb" "" "./c2m -I$srcdir/c-benchmarks -I. $bench.c -eb $arg" "$expect_out" "$inputf" $first
  #  run "c2m -ei" "" "./c2m -I$srcdir/c-benchmarks -I. $bench.c -ei $arg" "$expect_out" "$inputf"
  rm -f ./a.out
}

start_bench_num=$1
if echo $start_bench_num | grep -E [0-9]+ >/dev/null; then
  shift
elif test x$start_bench_num = x || (echo $start_bench_num | grep -E -v [0-9]+ >/dev/null); then
  start_bench_num=0
fi
short=$1

bench_num=0
for bench in array binary-trees except funnkuch-reduce hash hash2 heapsort lists matrix method-call mandelbrot nbody sieve spectral-norm strcat  # ackermann fib random 
do
    if test $bench_num -ge $start_bench_num; then
	b=$srcdir/$bench
	if test -f $b.arg; then arg=`sh $b.arg`; else arg=; fi
	echo "+++++ $bench_num:$bench $arg +++++"
	runbench $b $arg
    fi
    bench_num=`expr $bench_num + 1`
done

echo ============AVERAGE:=========
IFS=$'\n'
for i in `awk -F: '{print $1}' $temp3|sort|uniq`; do
    unset IFS
    s="$i:"
    $NECHO "$s"
    skip ${#s} 53
    awk -F: -v name="$i" "name==\$1 {f = f + \$2; n++} END {printf \"%0.2fx\n\", f / n;}" < $temp3
done

echo ============GEOMEAN:=========
IFS=$'\n'
for i in `awk -F: '{print $1}' $temp3|sort|uniq`; do
    unset IFS
    s="$i:"
    $NECHO "$s"
    skip ${#s} 53
    awk -F: -v name="$i" "BEGIN {f = 1.0} name==\$1 {f = f * \$2; n++} END {printf \"%0.2fx\n\", f ^  (1.0/n);}" < $temp3
done

rm -f $tempc $temp $temp2 $temp3 $errf ./a.out.wasm ./a.out.js
