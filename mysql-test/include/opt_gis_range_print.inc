#
# SUMMARY
#   MDEV-40553: printing of GIS ranges, shared between opt_trace.test
#   (dumps range_scan_alternatives from the optimizer trace) and
#   opt_context_store_stats.test (dumps the recorded optimizer context).
#   Both files run the exact same queries over the exact same fixture;
#   only the way each range is dumped differs, see
#   include/opt_gis_range_print_dump.inc.
#
# PARAMETERS
#   $opt_gis_range_print_use_trace  See include/opt_gis_range_print_dump.inc
#
# EXAMPLE
#   opt_trace.test, opt_context_store_stats.test
#
create table t1 (
  id int,
  g1 geometry not null,
  g2 geometry,
  key k_prefix (g1(5)),
  key k_full (g1(200)),
  key k_null (g2(200))
) engine=myisam;

insert into t1 select seq, point(seq, seq), point(seq, seq) from seq_1_to_5;
insert into t1 values (6, point(6,6), NULL);
analyze table t1;

--echo # The k_full prefix is long enough to hold every value this test
--echo # inserts (points and short linestrings), so the index holds the
--echo # whole value here. It is printed as a binary string, like any
--echo # other BLOB prefix.
explain
select id from t1 force index (k_full) where g1=point(2,2);
--source include/opt_gis_range_print_dump.inc

--echo # multiple ranges
explain
select id from t1 force index (k_full)
where g1=point(3,3) or g1=linestring(point(1,1), point(2,2));
--source include/opt_gis_range_print_dump.inc

--echo # NULL values and multiple ranges
explain
select id from t1 force index (k_null)
where g2=point(3,3) or g2 is null or g2=linestring(point(1,1), point(2,2));
--source include/opt_gis_range_print_dump.inc

--echo # The index holds only a prefix of the value.
--echo # It is printed as a binary string, like any other BLOB prefix.
explain
select id from t1 force index (k_prefix) where g1=point(2,2);
--source include/opt_gis_range_print_dump.inc

drop table t1;

--echo #
--echo # A SPATIAL index stores the MBR of the value. It is printed as the
--echo # WKT of the MBR together with the spatial relation the range uses.
--echo #

create table t2 (
  id int,
  g geometry not null,
  spatial key (g)
) engine=myisam;

insert into t2 select seq, point(seq, seq) from seq_1_to_20;
insert into t2 values
  (21, geomfromtext('LINESTRING(0 0, 5 8)')),
  (22, geomfromtext('POLYGON((0 0, 0 3, 3 3, 3 0, 0 0))'));
analyze table t2;

--echo # mbrcontains(value, column)
explain
select id from t2 where mbrcontains(geomfromtext('POLYGON((0 0,0 3,3 3,3 0,0 0))'), g);
--source include/opt_gis_range_print_dump.inc

--echo # mbrwithin(column, value)
explain
select id from t2 where mbrwithin(g, geomfromtext('LINESTRING(1 1, 9 12)'));
--source include/opt_gis_range_print_dump.inc

--echo # The other spatial relations
explain
select id from t2 where mbrcontains(g, geomfromtext('POINT(2 2)'));
--source include/opt_gis_range_print_dump.inc

explain
select id from t2 where mbrequals(g, geomfromtext('POINT(7 7)'));
--source include/opt_gis_range_print_dump.inc

explain
select id from t2 where mbrintersects(g, geomfromtext('POLYGON((0 0,0 2,2 2,2 0,0 0))'));
--source include/opt_gis_range_print_dump.inc

--echo # touches()/crosses()/overlaps() (and their MBRxxx aliases, where one
--echo # exists) all use the mbrintersects relation
explain
select id from t2 where mbrtouches(g, geomfromtext('POLYGON((0 0,0 2,2 2,2 0,0 0))'));
--source include/opt_gis_range_print_dump.inc

explain
select id from t2 where crosses(g, geomfromtext('POLYGON((0 0,0 2,2 2,2 0,0 0))'));
--source include/opt_gis_range_print_dump.inc

explain
select id from t2 where mbroverlaps(g, geomfromtext('POLYGON((0 0,0 2,2 2,2 0,0 0))'));
--source include/opt_gis_range_print_dump.inc

--echo # MBRDISJOINT: the handler's records_in_range() cannot estimate this
--echo # relation and returns HA_POS_ERROR, so the range optimizer discards
--echo # this index as a candidate before it ever traces its ranges -- the
--echo # trace shows the index with no further detail. The range text is
--echo # still produced (and recorded) when the optimizer separately asks
--echo # the handler for a cost estimate, see multi_range_read_info_const().
explain
select id from t2 where mbrdisjoint(g, geomfromtext('POLYGON((0 0,0 2,2 2,2 0,0 0))'));
--source include/opt_gis_range_print_dump.inc

drop table t2;
