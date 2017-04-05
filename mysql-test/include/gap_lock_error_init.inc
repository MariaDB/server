eval CREATE TABLE gap1 (id1 INT, id2 INT, id3 INT, c1 INT, value INT,
                      PRIMARY KEY (id1, id2, id3),
                      INDEX i (c1)) ENGINE=$engine;
CREATE TABLE gap2 like gap1;
eval CREATE TABLE gap3 (id INT, value INT,
                      PRIMARY KEY (id),
                      UNIQUE KEY ui(value)) ENGINE=$engine;
eval CREATE TABLE gap4 (id INT, value INT,
                      PRIMARY KEY (id)) ENGINE=$engine
                      PARTITION BY HASH(id) PARTITIONS 2;
--disable_query_log
let $max = 1000;
let $i = 1;
while ($i <= $max) {
  eval INSERT INTO gap1 (id1, id2, id3, c1, value)
       VALUES ($i div 2, $i div 10, $i, $i, $i);
  eval INSERT INTO gap2 (id1, id2, id3, c1, value)
       VALUES ($i div 2, $i div 10, $i, $i, $i);
  inc $i;
}
--enable_query_log

insert into gap3 values (1,1), (2,2),(3,3),(4,4),(5,5);
insert into gap4 values (1,1), (2,2),(3,3),(4,4),(5,5);
