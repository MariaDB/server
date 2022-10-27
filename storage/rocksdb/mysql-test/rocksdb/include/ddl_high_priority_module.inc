###############################################################################
# This file plays as a function/module for ddl_high_priority test
#
# Usage: set the following variables before including
#
#       $use_sys_var: whether using sys_var or syntax to trigger high_priority
#              value: 0/1
#
#         $con_block: a blocking connection
#              value: con1/con2/default
#
#          $con_kill: a connection that will attempt to kill $con_blocking
#              value: con1/con2/default
#
#               $cmd: a regular command to evaluate (to use with sys var)
#              value: sql command
#
# $high_priority_cmd: a high_priority command to evaluate
#              value: sql command
#
#       $should_kill: Expect the con_block to be killed or not
#              value: 0/1
#
#    $recreate_table: Should recreate the test table or not
#              value: 0/1
#
#       $throw_error: whether a command will throw lock_wait_timeout error.
#                     Note, optimize table catches all errors.
#              value: 0/1
###############################################################################

##
## Print out the parameters of the test set
## (useful for debugging)
##
--echo
--echo ## Test parameters:
--echo ## use_sys_var       = $use_sys_var
--echo ## con_block         = $con_block
--echo ## con_kill          = $con_kill
--echo ## cmd               = $cmd
--echo ## high_priority_cmd = $high_priority_cmd
--echo ## should_kill       = $should_kill
--echo ## recreate_table    = $recreate_table
--echo ## throw_error       = $throw_error
--echo


##
## Setup
##

connection default;

# create con1
connect (con1,localhost,test_user1,,test,,);

if ($recreate_table) {
  # create t1
  --disable_warnings
  drop table if exists t1;
  --enable_warnings
  create table t1 (i int);
  show create table t1;
  insert into t1 values (1), (2), (3);
}

##
## Testing
##

--echo connection: $con_block
--connection $con_block
--eval $blocking_sql

--echo connection: $con_kill
--connection $con_kill
set lock_wait_timeout = 0.02;
set high_priority_lock_wait_timeout = 0.02;

describe t1;

--echo connection: default (for show processlist)
connection default;
--echo # both $con_block and $con_kill exist
--replace_column 1 <Id> 3 <Host> 5 <Command> 6 <Time> 7 <State> 8 <Info> 9 <RExam> 10 <RSent> 11 <TID>
show processlist;

--echo connection: $con_kill
--connection $con_kill

# command will fail without high_priority
if ($throw_error) {
  --error ER_LOCK_WAIT_TIMEOUT
  --eval $cmd
}

if (!$throw_error) {
  --eval $cmd
}

if ($use_sys_var) {
  set high_priority_ddl = 1;
  select @@high_priority_ddl;

  # non-supported command will timeout
  --error ER_LOCK_WAIT_TIMEOUT
  lock tables t1 write;

  if (!$should_kill) {
    # regular user ddl will fail regardless of high_priority_ddl being on
    --error ER_LOCK_WAIT_TIMEOUT
    --eval $cmd
  }

  if ($should_kill) {
    --eval $cmd
  }

  # reset high_priority_ddl
  set high_priority_ddl = 0;
}

if (!$use_sys_var) {
  if (!$should_kill) {
    # regular user ddl will fail regardless of high_priority being on
    --error ER_LOCK_WAIT_TIMEOUT
    --eval $high_priority_cmd
  }

  if ($should_kill) {
    --eval $high_priority_cmd
  }
}

--echo connection: default (for show processlist)
connection default;
--replace_column 1 <Id> 3 <Host> 5 <Command> 6 <Time> 7 <State> 8 <Info> 9 <RExam> 10 <RSent> 11 <TID>
show processlist;

disconnect con1;
