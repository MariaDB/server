set -e

# Initially loads a chunk of data.
# Then start loading another chunk of data,
# while simultaneously running a backup

suite/rocksdb_hotbackup/include/load_data.sh 2>&1
suite/rocksdb_hotbackup/include/load_data.sh 2>&1 &
suite/rocksdb_hotbackup/include/stream_run.sh 2>&1
