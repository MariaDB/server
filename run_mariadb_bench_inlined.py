import json
import subprocess
import os
import time
import sys
import re
import string
from pathlib import Path

debug = 0

funcs = [
    # "memcpy_threshold_8byte_mov_avx_fallback", requires power of 2 alignment
    "memcpy_manual_avx_loop_aligned",
    "memcpy_manual_avx_loop_unaligned",
    "memcpy_manual_sse_loop",
    "memcpy_threshold16",
    "memcpy_rep_movsq",
    "memcpy_aligned<8>",
    "memcpy_aligned<16>",
    "memcpy_aligned<avx>",  # 32-byte aligned
    "memcpy",
    "memcpy_best_aligned",
    "memcpy_best_unaligned",
    "my_exact_unaligned_memcpy",
    "memcpy_switch_inline_aligned",
    "memcpy_threshold_builtins16",
    "memcpy_threshold_builtins8",
    "memcpy_threshold_builtins_avx",
]

engines = ["InnoDB", "MyISAM"]
sizes = [1024, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256]

header_path = "include/my_memcpy_bench.h"
table_cc_path = "sql/table.cc"
table_h_path = "sql/table.h"

def sed(file, pattern, subst):
    p = Path(file)
    old = p.read_text()

    new = re.sub(pattern, subst, old, flags=re.M)

    if old != new:
        p.write_text(new)

def replace_macro_and_alignment(func_name):
    align8 = "8" in func_name or func_name == "memcpy" or "unaligned" in func_name

    mult = '4' if 'avx' in func_name else '1' if align8 else '2'
    align_val = f"({mult}*ALIGN_MAX_UNIT)"
    reclength = 'reclength' if func_name == "memcpy" else 'rec_buff_length'

    if func_name.startswith("memcpy_aligned<"):
        func_name = "memcpy_aligned<RECORD_ALIGNMENT>"

    sed(header_path,
        r'(?<=^#define )copy_record_func\(.*$',
        f'copy_record_func(t, dst, src) {func_name}(dst, src, (t)->s->{reclength})'
    )

    sed(table_h_path,r'(?<=^#define RECORD_ALIGNMENT ).*$', align_val)



def run_cmd(cmd, cwd=None, capture=False, critical=True,**kwargs):
    kwargs.setdefault('stdout', sys.stdout if not capture and debug else subprocess.PIPE)
    kwargs.setdefault('stderr', sys.stderr if debug else subprocess.PIPE)

    res = subprocess.run(cmd, shell=True, cwd=cwd, **kwargs)
    if res.returncode and critical:
        print('Failed to run command', cmd)
        print(res.stdout.decode('utf-8').strip() if res.stdout else '')
        print(res.stderr.decode('utf-8').strip() if res.stderr else '')
        raise Exception()
    if capture:
        return res.returncode, res.stdout.decode('utf-8').strip()
    else:
        return res.returncode

def kill_mariadbd():
    run_cmd("pkill -9 mariadbd", critical=False)
    time.sleep(1)

def start_mariadbd():
    env = '_RR_TRACE_DIR=/home/nik/work/mariadb/bld/mysql-test/mysqld.1.rr' if debug else ''
    dbg = 'rr record' if debug else ''
    cmd = f"{env} taskset -c 0-15 {dbg} ./bld_gemini/sql/mariadbd --no-defaults --datadir=/tmp/mariadb_data_tmpfs --innodb-buffer-pool-size=4G --socket=/tmp/mariadb_bench.sock --skip-grant-tables > /tmp/mariadb_data_tmpfs/mariadbd.log 2>&1 &"
    subprocess.Popen(cmd, shell=True,
                     stdout=sys.stdout if debug else subprocess.PIPE,
                     stderr=sys.stderr if debug else subprocess.PIPE)
    
    for _ in range(10):
        if run_cmd("./bld_gemini/client/mariadb -u root --socket=/tmp/mariadb_bench.sock -e 'SELECT 1'", critical=False) == 0:
            return True
        time.sleep(1)
    return False

def init_datadir():
    run_cmd("rm -rf /tmp/mariadb_data_tmpfs/*", critical=False)
    run_cmd("./scripts/mariadb-install-db --no-defaults --builddir=. --datadir=/tmp/mariadb_data_tmpfs --force --verbose", cwd="bld_gemini")

def build(target):
    return run_cmd('ninja ' + target, cwd="bld_gemini")


if __name__ == "__main__":
    replace_macro_and_alignment(funcs[0])
    os.makedirs('bld_gemini', exist_ok=True)
    print("Running initial cmake... ")
    run_cmd('cmake .. -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS="-O3 -march=native -g" \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_C_FLAGS="-O3 -march=native -g" -G Ninja', cwd="bld_gemini")
    print("Running initial ninja...")
    build("minbuild provider_bzip2 provider_lz4 provider_lzma provider_snappy")

    print("Initializing datadir...")
    kill_mariadbd()
    init_datadir()

    for func in funcs:
        if func == "memcpy_threshold_unaligned_avx":
            continue

        print(f"\n--- {func} ---", flush=True)
        replace_macro_and_alignment(func)

        build("mariadbd")

        if not start_mariadbd():
            print(f"  Server failed to start for {func} {engine}/{size}. Skipping.", flush=True)
            sys.exit(1)

        if debug:
            print("Build done.")
        for engine in engines:
            for size in sizes:
                if debug:
                    print(f"{engine} --- {size}")

                run_cmd(f"./bld_gemini/client/mariadb -u root --socket=/tmp/mariadb_bench.sock -e \"DROP DATABASE IF EXISTS bench_db; CREATE DATABASE bench_db;\"")
                run_cmd(f"./bld_gemini/client/mariadb -u root --socket=/tmp/mariadb_bench.sock bench_db -e \"CREATE TABLE t1 (id INT PRIMARY KEY AUTO_INCREMENT, val VARCHAR({size})) ENGINE={engine} /*WITH SYSTEM VERSIONING*/;\"")

                # Insert data
                run_cmd(f'./bld_gemini/client/mariadb -u root --socket=/tmp/mariadb_bench.sock bench_db -e "insert t1 select seq, \'{'a'*size}\' from seq_1_to_1000000"')
                
                # Actual run. Run it 100 times.
                success = True


                # Use a single connection for the loop and get_stats
                # create a script to execute
                with open("/tmp/bench_script.sql", "w") as f_sql:
                    f_sql.write("USE bench_db;\n")
                    f_sql.write(f"UPDATE t1 set val= '{'0'*size}';\n") # Warmup
                    f_sql.write("SELECT CONCAT(@@memcpy_time, ' ', @@memcpy_calls) AS m1;\n")
                    for c in string.ascii_lowercase + string.hexdigits:
                        f_sql.write(f"UPDATE t1 set val= '{c*size}';\n")
                    # for c in string.ascii_lowercase[1:2]:
                    #     f_sql.write(f"REPLACE t1 select id, '{c*size}' from t1;\n")
                    f_sql.write("SELECT CONCAT(@@memcpy_time, ' ', @@memcpy_calls) AS m2;\n")
                rc, out = run_cmd("./bld_gemini/client/mariadb -u root --socket=/tmp/mariadb_bench.sock -s -N < /tmp/bench_script.sql",
                                  capture=True, critical=False)
                
                start_calls = 0
                start_ns = 0
                end_calls = 0
                end_ns = 0
                
                if rc == 0:
                    lines = [l for l in out.strip().split('\n') if l.strip()]
                    if len(lines) == 2:
                        try:
                            start_ns, start_calls = map(int, lines[0].split())
                            end_ns, end_calls = map(int, lines[1].split())
                        except:
                            success = False; print('ERROR rc=', rc, 'out=', out.strip(), flush=True)
                    else:
                        success = False; print('ERROR rc=', rc, 'out=', out.strip(), flush=True)
                else:
                    success = False; print('ERROR rc=', rc, 'out=', out.strip(), flush=True)
                if success and end_calls > start_calls:
                    diff_ns = end_ns - start_ns
                    diff_calls = end_calls - start_calls
                    ns_per_call = diff_ns / diff_calls
                    
                    print(f"{engine}/{size}: {ns_per_call:.2f} cycles/call ({diff_calls} calls)", flush=True)
                else:
                    print(f"  Bench failed for {engine}/{size}. (success={success} end_calls={end_calls} start_calls={start_calls})", flush=True)

    kill_mariadbd()
