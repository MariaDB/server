#!/bin/bash

set -e

# Insert 100 batches of 100 records each to a table with following schema:
# create table db1.t1 (
#   `id` int(10) not null auto_increment,
#   `k` int(10),
#   `data` varchar(2048),
#   primary key (`id`),
#   key (`k`)
# ) engine=innodb;

MAX_INSERTS=100
MAX_ROWS_PER_INSERT=100

insertData() {
  for ((i=1; i<=$MAX_INSERTS; i++));
  do
      stmt='INSERT INTO db1.t1 values'
      for ((j=1; j<=$MAX_ROWS_PER_INSERT; j++));
      do
          k=$RANDOM
          data=$(head -c 2048 /dev/urandom|tr -cd 'a-zA-Z0-9')
          stmt=$stmt' (NULL, '$k', "'$data'")'
          if [ $j -lt $MAX_ROWS_PER_INSERT ]; then
              stmt=$stmt','
          fi
      done
      stmt=$stmt';'
      $MYSQL --defaults-group-suffix=.1 -e "$stmt"
  done
}

NUM_PARALLEL_INSERTS=25
pids=()
for ((k=1; k<=$NUM_PARALLEL_INSERTS; k++));
do
  insertData &
  pids+=($!)
done
for ((k=1; k<=$NUM_PARALLEL_INSERTS; k++));
do
  wait ${pids[k]}
done
