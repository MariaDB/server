if [ "$STREAM_TYPE" == 'wdt' ]; then
  which wdt >/dev/null 2>&1
  if [ $? -ne 0 ]; then
    # change to tar if wdt is not installed
    STREAM_TYPE='tar'
  fi
fi

set -e

# Takes a full backup from server_1 to server_2
# using myrocks_hotbackup streaming

checkpoint_dir="${MYSQLTEST_VARDIR}/checkpoint"
backup_dir="${MYSQLTEST_VARDIR}/backup"
dest_data_dir="${MYSQLTEST_VARDIR}/mysqld.2/data/"

mysql_dir=$(echo $MYSQL | awk '{print $1}' | xargs dirname)
PATH=$mysql_dir:$PATH

mkdir -p $checkpoint_dir
rm -rf $checkpoint_dir/*

mkdir -p $backup_dir
rm -rf $backup_dir/*
# delete and recreate the dest dir to make sure all hidden files
# and directories (such as .rocksdb) are blown away
rm -rf $dest_data_dir/
mkdir $dest_data_dir

COPY_LOG="${MYSQL_TMP_DIR}/myrocks_hotbackup_copy_log"

if [ "$STREAM_TYPE" == 'tar' ]; then
  BACKUP_CMD="$MYSQL_MYROCKS_HOTBACKUP --user='root' --port=${MASTER_MYPORT} \
    --stream=tar --checkpoint_dir=$checkpoint_dir 2> \
    $COPY_LOG | tar -xi -C $backup_dir"
elif [ "$STREAM_TYPE" == 'xbstream' ]; then
  BACKUP_CMD="$MYSQL_MYROCKS_HOTBACKUP --user='root' --port=${MASTER_MYPORT} \
    --stream=xbstream --checkpoint_dir=$checkpoint_dir 2> \
    $COPY_LOG | xbstream -x \
    --directory=$backup_dir"
elif [ "$STREAM_TYPE" == "xbstream_socket" ]; then
  BACKUP_CMD="$MYSQL_MYROCKS_HOTBACKUP --user='root' --socket=${MASTER_MYSOCK} \
    --stream=xbstream --checkpoint_dir=$checkpoint_dir 2> \
    $COPY_LOG | xbstream -x \
    --directory=$backup_dir"
else
  BACKUP_CMD="$MYSQL_MYROCKS_HOTBACKUP --user='root' --stream=wdt \
    --port=${MASTER_MYPORT} --destination=localhost --backup_dir=$backup_dir \
    --avg_mbytes_per_sec=10 --interval=5 \
    --extra_wdt_sender_options='--block_size_mbytes=1' \
    --checkpoint_dir=$checkpoint_dir 2> \
    $COPY_LOG"
fi

echo "myrocks_hotbackup copy phase"
eval "$BACKUP_CMD"
if [ $? -ne 0 ]; then
  tail $COPY_LOG
  exit 1
fi

mkdir ${backup_dir}/test      # TODO: Fix skipping empty directories

MOVEBACK_LOG="${MYSQL_TMP_DIR}/myrocks_hotbackup_moveback_log"

echo "myrocks_hotbackup move-back phase"
$MYSQL_MYROCKS_HOTBACKUP --move_back --datadir=$dest_data_dir \
  --rocksdb_datadir=$dest_data_dir/.rocksdb \
  --rocksdb_waldir=$dest_data_dir/.rocksdb \
  --backup_dir=$backup_dir > $MOVEBACK_LOG 2>&1

if [ $? -ne 0 ]; then
  tail $MOVEBACK_LOG
  exit 1
fi
