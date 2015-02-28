--disable_warnings
DROP TABLE IF EXISTS graph_base;
DROP TABLE IF EXISTS graph;
DROP TABLE IF EXISTS graph2;
--enable_warnings

--echo Performing OQGraph General test suite for ENGINE=$oqgraph_use_table_type

# Create the backing store
eval CREATE TABLE graph_base (
    from_id INT UNSIGNED NOT NULL,
    to_id INT UNSIGNED NOT NULL,
    PRIMARY KEY (from_id,to_id),
    INDEX (to_id)
  ) ENGINE= $oqgraph_use_table_type ;

# Since late June 2014 OQGraph supports 'assisted discovery' as per https://mariadb.atlassian.net/browse/MDEV-5871
CREATE TABLE graph ENGINE=OQGRAPH DATA_TABLE='graph_base' ORIGID='from_id', DESTID='to_id';

# Regression for MDEV-5891
select * from graph;

#--
#-- ASCII art graph of this test data
#--          +-->(2)
#--   ( )<---+
#--   (1)
#--   ( )<---+
#--          +-->(3)<------->(4)
#--
#--   (7)<----------(5)<--------->(6)         (9)
#--  
#--         +--->(11)
#--         |      |
#--       (10)     |      
#--         ^      v
#--         +----(12)

INSERT INTO graph_base(from_id, to_id) VALUES (1,2), (2,1);
INSERT INTO graph_base(from_id, to_id) VALUES (1,3), (3,1);
INSERT INTO graph_base(from_id, to_id) VALUES (3,4), (4,3);
INSERT INTO graph_base(from_id, to_id) VALUES (5,6), (6,5);

#-- extra unidirected node
INSERT INTO graph_base(from_id, to_id) VALUES (5,7);

#-- isolated node with no loop - disallowed
#-- so origid 8 below should return an empty rowset
#-- INSERT INTO graph_base(from_id, to_id) VALUES (8,NULL);

#-- isolated node with a (undirected) loop
#-- we have no way of representing a directed loop on an isolated node, is this valid in pure graph theory?
INSERT INTO graph_base(from_id, to_id) VALUES (9,9);

#-- directed _cyclic_ graph triangle?
INSERT INTO graph_base(from_id, to_id) VALUES (10,11);
INSERT INTO graph_base(from_id, to_id) VALUES (11,12);
INSERT INTO graph_base(from_id, to_id) VALUES (12,10);

--echo # Return all edges
#-- we note that when weight is NULL it defaults to 1
SELECT * FROM graph;
--echo # Currently count should be 13
SELECT count(*) FROM graph;

--echo # Return all edges when latch is NULL - this is different to latch='' and same as no where clause
SELECT * FROM graph where latch is NULL;

--echo # Return all vertices, and subsets of vertices
SELECT * FROM graph where latch='';
SELECT * FROM graph where latch='0';

--echo # Currently count should be 11

SELECT count(*) FROM graph where latch='';
#-- get a subset of vertices
SELECT * FROM graph where latch='' and linkid = 2;
SELECT * FROM graph where latch='' and (linkid > 2 and linkid < 6);
SELECT * FROM graph where latch='' and linkid = NULL;
SELECT * FROM graph where latch='' and linkid = 666;

#-- Query out-edges for vertex (no_search AND origid=N)
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = 1;
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = 2;
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = 4;
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = 9;
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = 10;
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = NULL;
SELECT origid as `from`, linkid as `to` FROM graph where latch='' and origid = 666;

#-- Query in-edges for vertex (no_search AND destid=N)
#-- linkid will have the other end
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = 1;
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = 2;
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = 4;
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = 9;
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = 10;
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = NULL;
SELECT linkid as `from`, destid as `to` FROM graph where latch='' and destid = 666;

# The following returns a result that makes no sense...
#-- what happens when we combined orig and dest?
#-- Bug https://bugs.launchpad.net/oqgraph/+bug/1195778
#SELECT * FROM graph where latch='' and origid = 1;
#SELECT * FROM graph where latch='' and destid = 2;
#SELECT * FROM graph where latch='' and origid=1 and destid = 2;

SELECT * FROM graph where latch='0';
SELECT count(*) FROM graph where latch='0';
SELECT * FROM graph where latch='0' and linkid = 2;
SELECT * FROM graph where latch='0' and (linkid > 2 and linkid < 6);
SELECT origid as `from`, linkid as `to` FROM graph where latch='0' and origid = 1;
SELECT origid as `from`, linkid as `to` FROM graph where latch='0' and origid = 2;
SELECT origid as `from`, linkid as `to` FROM graph where latch='0' and origid = 4;
SELECT origid as `from`, linkid as `to` FROM graph where latch='0' and origid = 9;
SELECT origid as `from`, linkid as `to` FROM graph where latch='0' and origid = 10;
SELECT linkid as `from`, destid as `to` FROM graph where latch='0' and destid = 1;
SELECT linkid as `from`, destid as `to` FROM graph where latch='0' and destid = 2;
SELECT linkid as `from`, destid as `to` FROM graph where latch='0' and destid = 4;
SELECT linkid as `from`, destid as `to` FROM graph where latch='0' and destid = 9;
SELECT linkid as `from`, destid as `to` FROM graph where latch='0' and destid = 10;

--echo # Breadth-first search tests
#-- We are asking "Is there a path from node 'origid' to (all) other nodes?"
#-- We return a row for each other node that is reachable, with its id in 'linkid'
#-- and the weight calculated as "How many _directed_ hops to get there"
#-- If there is no path from origid to another node then there is no row for that linkid
#-- We include 'origid' in the set of reachable nodes i.e. as a 'loop', with weight 0
#-- 'seq' is the counted distance of the search, thus, the loop link will always have seq 1
#--       if there are two reachable neighbours, they will have seq 2,3 and so on
#-- linkid is the other end
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 4;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 5;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 6;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 7;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 8;  # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 9;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 10;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 11;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 12;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 666; # <-- note, should return nothing
#-- The above results can then be filtered by weight, so the results should be a subset for the corresponding origid above
#-- so effectively, `AND weight=1` returns the neighbours of origid in linkid
#<----- orig test harness - still returns (breadth_first	1	NULL	1	3	3), (breadth_first	1	NULL	1	2	2)
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 1 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 2 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 3 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 4 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 5 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 6 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 7 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 8 AND weight = 1; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 9 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 10 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 11 AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 12 AND weight = 1;
#-- so effectively, `count(... AND weight=1)` returns the number of _reachable_ immediate neighbours
#-- included because it allows human to quickly eyeball against the visual ASCII graph for correctness...
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 1 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 2 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 3 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 4 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 5 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 6 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 7 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 8 AND weight = 1; # <-- note, should return nothing
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 9 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 10 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 11 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = 'breadth_first' AND origid = 12 AND weight = 1;
#-- so effectively, `AND weight=2` returns the second-level neighbours of origid in linkid
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 1 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 2 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 3 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 4 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 5 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 6 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 7 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 8 AND weight = 2; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 9 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 10 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 11 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 12 AND weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 1 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 2 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 3 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 4 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 5 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 6 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 7 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 8 AND weight = 3; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 9 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 10 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 11 AND weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 12 AND weight = 3;

SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 1 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 2 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 3 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 4 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 5 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 6 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 7 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 8 AND (weight = 1 or weight = 2); # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 9 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 10 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 11 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = 12 AND (weight = 1 or weight = 2);

#-- now do it in reverse - using destid find originating vertices
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 4;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 5;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 6;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 7;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 8; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 9;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 10;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 11;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 12;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 1 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 2 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 3 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 4 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 5 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 6 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 7 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 8 and weight = 1; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 9 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 10 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 11 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 12 and weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 1 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 2 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 3 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 4 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 5 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 6 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 7 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 8 and weight = 2; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 9 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 10 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 11 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 12 and weight = 2;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 1 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 2 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 3 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 4 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 5 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 6 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 7 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 8 and weight = 3; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 9 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 10 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 11 and weight = 3;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = 12 and weight = 3;

#-- These return empty sets - origid or destid must be specified and non null to get a result set
SELECT * FROM graph WHERE latch = 'breadth_first' AND origid = NULL;
SELECT * FROM graph WHERE latch = 'breadth_first' AND destid = NULL;
SELECT * FROM graph WHERE latch = 'breadth_first' AND weight = 1;
SELECT * FROM graph WHERE latch = 'breadth_first';

#-- Repeat the above with legacy string
SELECT * FROM graph WHERE latch = '2' AND origid = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 4;
SELECT * FROM graph WHERE latch = '2' AND origid = 5;
SELECT * FROM graph WHERE latch = '2' AND origid = 6;
SELECT * FROM graph WHERE latch = '2' AND origid = 7;
SELECT * FROM graph WHERE latch = '2' AND origid = 8; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND origid = 9;
SELECT * FROM graph WHERE latch = '2' AND origid = 10;
SELECT * FROM graph WHERE latch = '2' AND origid = 11;
SELECT * FROM graph WHERE latch = '2' AND origid = 12;
SELECT * FROM graph WHERE latch = '2' AND origid = 666; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND origid = 1 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 2 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 3 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 4 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 5 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 6 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 7 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 8 AND weight = 1; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND origid = 9 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 10 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 11 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 12 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 1 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 2 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 3 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 4 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 5 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 6 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 7 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 8 AND weight = 1; # <-- note, should return nothing
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 9 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 10 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 11 AND weight = 1;
SELECT count(*) FROM graph WHERE latch = '2' AND origid = 12 AND weight = 1;
SELECT * FROM graph WHERE latch = '2' AND origid = 1 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 2 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 3 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 4 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 5 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 6 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 7 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 8 AND weight = 2; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND origid = 9 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 10 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 11 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 12 AND weight = 2;
SELECT * FROM graph WHERE latch = '2' AND origid = 1 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 2 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 3 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 4 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 5 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 6 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 7 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 8 AND weight = 3; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND origid = 9 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 10 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 11 AND weight = 3;
SELECT * FROM graph WHERE latch = '2' AND origid = 12 AND weight = 3;

SELECT * FROM graph WHERE latch = '2' AND origid = 1 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 2 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 3 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 4 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 5 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 6 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 7 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 8 AND (weight = 1 or weight = 2); # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND origid = 9 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 10 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 11 AND (weight = 1 or weight = 2);
SELECT * FROM graph WHERE latch = '2' AND origid = 12 AND (weight = 1 or weight = 2);

SELECT * FROM graph WHERE latch = '2' AND destid = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 4;
SELECT * FROM graph WHERE latch = '2' AND destid = 5;
SELECT * FROM graph WHERE latch = '2' AND destid = 6;
SELECT * FROM graph WHERE latch = '2' AND destid = 7;
SELECT * FROM graph WHERE latch = '2' AND destid = 8;  # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND destid = 9;
SELECT * FROM graph WHERE latch = '2' AND destid = 10;
SELECT * FROM graph WHERE latch = '2' AND destid = 11;
SELECT * FROM graph WHERE latch = '2' AND destid = 12;
SELECT * FROM graph WHERE latch = '2' AND destid = 1 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 2 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 3 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 4 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 5 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 6 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 7 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 8 and weight = 1; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND destid = 9 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 10 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 11 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 12 and weight = 1;
SELECT * FROM graph WHERE latch = '2' AND destid = 1 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 2 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 3 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 4 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 5 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 6 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 7 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 8 and weight = 2; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND destid = 9 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 10 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 11 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 12 and weight = 2;
SELECT * FROM graph WHERE latch = '2' AND destid = 1 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 2 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 3 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 4 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 5 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 6 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 7 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 8 and weight = 3; # <-- note, should return nothing
SELECT * FROM graph WHERE latch = '2' AND destid = 9 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 10 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 11 and weight = 3;
SELECT * FROM graph WHERE latch = '2' AND destid = 12 and weight = 3;

#-- These return empty sets - origid must be specified and non null to get a result set
SELECT * FROM graph WHERE latch = '2' AND origid = NULL;
SELECT * FROM graph WHERE latch = '2' AND destid = NULL;
SELECT * FROM graph WHERE latch = '2' AND weight = 1;
SELECT * FROM graph WHERE latch = '2';


--echo # Dijkstras algorithm tests
#-- We ask 'What is the shortest path (if any) between 'origid' and 'destid'
#-- This returns the number of directed hops +1 (for the starting node)
#-- 'weight' is NULL for the starting point, or 1
#-- 'linkid' is the way point id
#-- 'seq' is the distance of the waypoint from the start (counting from zero)
#-- the default order returned is waypoints out from the start
#-- zero hop (1 row)
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1 AND destid=1;
#-- one hop
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1 AND destid=2;
#-- one hop in reverse
SELECT * FROM graph WHERE latch='dijkstras' AND origid=2 AND destid=1;
#-- two hops (via 3)
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1 AND destid=4;
#-- two hops in reverse direction
SELECT * FROM graph WHERE latch='dijkstras' AND origid=4 AND destid=1;
#-- no result (no connection)
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1 AND destid=5;
#-- no result (no destination exists)
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1 AND destid=666;

#-- one hop on a unidirected link
SELECT * FROM graph WHERE latch='dijkstras' AND origid=5 AND destid=7;
#-- zero hop in reverse direction on a unidirected link
SELECT * FROM graph WHERE latch='dijkstras' AND origid=7 AND destid=5;

#-- Trickery - what about the cyclic loop?
SELECT * FROM graph WHERE latch='dijkstras' AND origid=10 AND destid=11;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=10 AND destid=12;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=11 AND destid=10;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=11 AND destid=12;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=12 AND destid=10;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=12 AND destid=11;

#-- reachable vertices
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=2;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=3;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=4;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=5;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=6;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=7;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=8; # <-- note, should return nothing
SELECT * FROM graph WHERE latch='dijkstras' AND origid=9;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=10;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=11;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=12;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=666; # <-- note, should return nothing

#-- originating vertices
SELECT * FROM graph WHERE latch='dijkstras' AND destid=1;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=2;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=3;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=4;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=5;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=6;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=7;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=8; # <-- note, should return nothing
SELECT * FROM graph WHERE latch='dijkstras' AND destid=9;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=10;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=11;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=12;

--echo # legacy string number
SELECT * FROM graph WHERE latch='1' AND origid=1 AND destid=1;
SELECT * FROM graph WHERE latch='1' AND origid=1 AND destid=2;
SELECT * FROM graph WHERE latch='1' AND origid=2 AND destid=1;
SELECT * FROM graph WHERE latch='1' AND origid=1 AND destid=4;
SELECT * FROM graph WHERE latch='1' AND origid=4 AND destid=1;
SELECT * FROM graph WHERE latch='1' AND origid=1 AND destid=5;
SELECT * FROM graph WHERE latch='1' AND origid=1 AND destid=666; # <-- note, should return nothing
SELECT * FROM graph WHERE latch='1' AND origid=5 AND destid=7;
SELECT * FROM graph WHERE latch='1' AND origid=7 AND destid=5;
SELECT * FROM graph WHERE latch='1' AND origid=10 AND destid=11;
SELECT * FROM graph WHERE latch='1' AND origid=10 AND destid=12;
SELECT * FROM graph WHERE latch='1' AND origid=11 AND destid=10;
SELECT * FROM graph WHERE latch='1' AND origid=11 AND destid=12;
SELECT * FROM graph WHERE latch='1' AND origid=12 AND destid=10;
SELECT * FROM graph WHERE latch='1' AND origid=12 AND destid=11;
SELECT * FROM graph WHERE latch='1' AND origid=1;
SELECT * FROM graph WHERE latch='1' AND origid=2;
SELECT * FROM graph WHERE latch='1' AND origid=3;
SELECT * FROM graph WHERE latch='1' AND origid=4;
SELECT * FROM graph WHERE latch='1' AND origid=5;
SELECT * FROM graph WHERE latch='1' AND origid=6;
SELECT * FROM graph WHERE latch='1' AND origid=7;
SELECT * FROM graph WHERE latch='1' AND origid=8; # <-- note, should return nothing
SELECT * FROM graph WHERE latch='1' AND origid=9;
SELECT * FROM graph WHERE latch='1' AND origid=10;
SELECT * FROM graph WHERE latch='1' AND origid=11;
SELECT * FROM graph WHERE latch='1' AND origid=12;
SELECT * FROM graph WHERE latch='1' AND origid=666; # <-- note, should return nothing
SELECT * FROM graph WHERE latch='1' AND destid=1;
SELECT * FROM graph WHERE latch='1' AND destid=2;
SELECT * FROM graph WHERE latch='1' AND destid=3;
SELECT * FROM graph WHERE latch='1' AND destid=4;
SELECT * FROM graph WHERE latch='1' AND destid=5;
SELECT * FROM graph WHERE latch='1' AND destid=6;
SELECT * FROM graph WHERE latch='1' AND destid=7;
SELECT * FROM graph WHERE latch='1' AND destid=8; # <-- note, should return nothing
SELECT * FROM graph WHERE latch='1' AND destid=9;
SELECT * FROM graph WHERE latch='1' AND destid=10;
SELECT * FROM graph WHERE latch='1' AND destid=11;
SELECT * FROM graph WHERE latch='1' AND destid=12;

#-- What if we add two equally valid two-hop paths?
#--
#--
#--     +--->(14)----------+
#--     |                  v
#--     |   +--->(11)---->(13)
#--     |   |      |
#--     +-(10)     |      
#--         ^      v
#--         +----(12)
#--
#-- We note it chooses 10,11,13 but will it always?
INSERT INTO graph_base(from_id, to_id) VALUES (11,13);
INSERT INTO graph_base(from_id, to_id) VALUES (10,14);
INSERT INTO graph_base(from_id, to_id) VALUES (14,13);
SELECT * FROM graph WHERE latch='dijkstras' AND origid=10 AND destid=13;
DELETE FROM graph_base where from_id=10 and to_id=11;
INSERT INTO graph_base(from_id, to_id) VALUES (10,15);
INSERT INTO graph_base(from_id, to_id) VALUES (15,13);
SELECT * FROM graph WHERE latch='dijkstras' AND origid=10 AND destid=13;
INSERT INTO graph_base(from_id, to_id) VALUES (10,11);
#-- We note is _appears_ to use the lowered valued node id if there are two equal paths
SELECT * FROM graph WHERE latch='dijkstras' AND origid=10 AND destid=13;

#-- add some extra and check
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1;
INSERT INTO graph_base(from_id, to_id) VALUES (21,22);
SELECT * FROM graph WHERE latch='dijkstras' AND origid=21;
SELECT * FROM graph WHERE latch='dijkstras' AND origid=22;
INSERT INTO graph_base(from_id, to_id) VALUES (4,17);
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1;
INSERT INTO graph_base(from_id, to_id) VALUES (4,16);
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1;
INSERT INTO graph_base(from_id, to_id) VALUES (17,18);
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1;
SELECT * FROM graph WHERE latch='dijkstras' AND destid=1;

--echo # Now we add a connection from 4->6 
INSERT INTO graph_base (from_id,to_id) VALUES (4,6);

--echo # And delete all references to node 5
DELETE FROM graph_base WHERE from_id=5;
DELETE FROM graph_base WHERE from_id=3 AND to_id=5;

--echo # which means there is a path in one direction only 1>3>4>6
SELECT * FROM graph WHERE latch='dijkstras' AND origid=1 AND destid=6;
--echo # but not 6>4>3>1 (so no result)
SELECT * FROM graph WHERE latch='dijkstras' AND origid=6 AND destid=1;

SELECT * FROM graph WHERE latch='1' AND origid=1 AND destid=6;
SELECT * FROM graph WHERE latch='1' AND origid=6 AND destid=1;


DELETE FROM graph_base;
FLUSH TABLES;
TRUNCATE TABLE graph_base;

DROP TABLE graph_base;
DROP TABLE graph;

#-- Reminder - the basic spec is at http://openquery.com/graph/doc
#--    Query edges stored in graph engine (latch=NULL)
#--    SELECT * FROM foo;
#--    Results:
#--        vertex id for origin of edge in origid column.
#--        vertex id for destination of edge in destid column.
#--        weight of edge in weight column.
#--    Essentially this returns the values (origid,destid pairs with optional weight) you put in, in this mode OQGRAPH looks very close to a real table. But it also does nothing special, it's just store/retrieve for those columns. The other columns will be returned as NULL.
#--     
#--    Query vertices stored in graph engine (latch=0)
#--    SELECT * FROM foo WHERE latch = 0;
#--    Results:
#--        vertex id in linkid column
#--     
#--    Query out-edges for vertex (latch=0 AND origid=N)
#--    SELECT * FROM foo WHERE latch = 0 AND origid = 2;
#--    Results:
#--        vertex id in linkid column
#--        edge weight in weight column
#--     
#--    Query in-edges for vertex (latch=0 AND destid=N)
#--    SELECT * FROM foo WHERE latch = 0 AND destid = 6;
#--    Results:
#--        vertex id in linkid column
#--        edge weight in weight column
#--     
#--    Dijkstra's shortest path algorithm (latch=1)
#--    Find shortest path:
#--    SELECT * FROM foo WHERE latch = 1 AND origid = 1 AND destid = 6;
#--      Results:
#--        latch, origid, destid are same as input.
#--        vertex id of the current step in linkid column.
#--        weight of traversed edge in weight column.
#--        step counter in seq column, so you can sort and use the result (starting at step 0).
#--        Example: SELECT GROUP_CONCAT(linkid ORDER BY seq) ...
#--
#--    Find reachable vertices:
#--    SELECT * FROM foo WHERE latch = 1 AND origid = 1;
#--      Results:
#--        latch, origid, destid are same as input.
#--        vertex id in linkid column.
#--        aggregate of weights in weight column.
#--     
#--    Find originating vertices:
#--    SELECT * FROM foo WHERE latch = 1 AND destid = 6;
#--      Results:
#--        latch, origid, destid are same as input.
#--        vertex id in linkid column.
#--        aggregate of weights in weight column.
#--     
#--    Breadth-first search (latch=2, assumes that each vertex is weight 1)
#--    Find shortest path:
#--    SELECT * FROM foo WHERE latch = 2 AND origid = 1 AND destid = 6;
#--    Results:
#--        vertex id in linkid column.
#--        weight column = 1 for each hop.
#--
#--    Find reachable vertices:
#--    SELECT * FROM foo WHERE latch = 2 AND origid = 1;
#--      Results:
#--        vertex id in linkid column.
#--        computed number of hops in weight column.
#--     
#--    Find originating vertices:
#--    SELECT * FROM foo WHERE latch = 2 AND destid = 6;
#--    Results:
#--        vertex id in linkid column.
#--        computed number of hops in weight column.


