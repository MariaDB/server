#!/bin/bash

set -e

COPY_LOG=$1
SIGNAL_FILE=$2
# Creating a table after myrocks_hotbackup reaches waiting loop

done=0
while : ; do
  wait=`tail -1 $COPY_LOG | grep 'Waiting until' | wc -l`
  if [ "$wait" -eq "1" ]; then
    break
  fi
  sleep 1
done
$MYSQL --defaults-group-suffix=.1 db1 -e "create table r10 (id int primary key ) engine=rocksdb"
touch $SIGNAL_FILE
