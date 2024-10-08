# Tests for the performance schema

# =============
# DOCUMENTATION
# =============

# Verify critical stages of a statement
#
# The tests are written with the following helpers:
# - include/stage_setup.inc
# - include/stage_cleanup.inc
#
# Helpers are intended to be used as follows.
#
# A Typical test t/stage_xxx.test will consist of:
# --source ../include/stage_setup.inc
# ... test specific payload ...
# --source ../include/stage_cleanup.inc
# and a t/stage_xxx-master.opt file
#
# ==============================
# HELPER include/stage_setup.inc
# ==============================

--source include/not_embedded.inc
--source include/have_perfschema.inc
--source include/no_protocol.inc

--disable_query_log

create user user1@localhost;
grant ALL on *.* to user1@localhost;
create user user2@localhost;
grant ALL on *.* to user2@localhost;
create user user3@localhost;
grant ALL on *.* to user3@localhost;
create user user4@localhost;
grant ALL on *.* to user4@localhost;

flush privileges;

# Save the setup

--disable_warnings
drop table if exists test.setup_actors;
drop table if exists test.t1;
--enable_warnings

create table test.t1(a varchar(64));

create table test.setup_actors as
  select * from performance_schema.setup_actors;

# Only instrument the user connections
truncate table performance_schema.setup_actors;
insert into performance_schema.setup_actors
  set host= 'localhost', user= 'user1', role= '%';
insert into performance_schema.setup_actors
  set host= 'localhost', user= 'user2', role= '%';
insert into performance_schema.setup_actors
  set host= 'localhost', user= 'user3', role= '%';
insert into performance_schema.setup_actors
  set host= 'localhost', user= 'user4', role= '%';

update performance_schema.threads set instrumented='NO';

# Only instrument a few events of each kind
update performance_schema.setup_instruments set enabled='YES', timed='YES';

# Start from a known clean state, to avoid noise from previous tests
flush tables;
flush global status;
truncate performance_schema.events_stages_summary_by_thread_by_event_name;
truncate performance_schema.events_stages_summary_global_by_event_name;
truncate performance_schema.events_stages_history;
truncate performance_schema.events_stages_history_long;
truncate performance_schema.events_statements_summary_by_thread_by_event_name;
truncate performance_schema.events_statements_summary_global_by_event_name;
truncate performance_schema.events_statements_history;
truncate performance_schema.events_statements_history_long;

--disable_warnings
drop procedure if exists dump_thread;
drop procedure if exists dump_one_thread;
--enable_warnings

delimiter $$;

create procedure dump_thread()
begin
  call dump_one_thread('user1');
  call dump_one_thread('user2');
  call dump_one_thread('user3');
  call dump_one_thread('user4');
end
$$

create procedure dump_one_thread(in username varchar(64))
begin
  declare my_thread_id int;
  declare my_statement_id int;

  set my_thread_id = (select thread_id from performance_schema.threads
                        where processlist_user=username);

  if (my_thread_id is not null) then
  begin
    # Dump the current statement for this thread
    select username, event_name, sql_text
      from performance_schema.events_statements_current
      where thread_id = my_thread_id;

    # Get the current statement
    set my_statement_id = (select event_id from
      performance_schema.events_statements_current
      where thread_id = my_thread_id);
      
    # Dump the stages for this statement
    select username, event_name, nesting_event_type
      from performance_schema.events_stages_current
      where thread_id = my_thread_id
      and nesting_event_id = my_statement_id
      order by event_id asc;
    # 1. Ignore query cache as it may not be enabled
    # 2. MDL wait consists of short 1 second waits (this is not configurable)
    #    repeated until lock_wait_timeout is reached. The stage is changed
    #    to Waiting and back every second. To have predictable result here
    #    the query filters all sequences of X, "Waiting for MDL", X,
    #    leaving just X.
    with h as (
      select event_id, username, event_name, nesting_event_type,
        lag(event_name) over (order by event_id) = lead(event_name) over (order by event_id)
        and event_name = "stage/sql/Waiting for stored function metadata lock" as v1,
        lag(event_name, 2) over (order by event_id) = event_name
        and lag(event_name, 1) over (order by event_id) = "stage/sql/Waiting for stored function metadata lock" as v2
        from performance_schema.events_stages_history
        where thread_id = my_thread_id
        and nesting_event_id = my_statement_id and
        event_name not like "%query cache%"
      ) select username, event_name, nesting_event_type from h where v1 is not true and v2 is not true
      order by event_id asc;
  end;
  else
    select username, "not found" as status;
  end if;
end
$$

delimiter ;$$

--enable_query_log
