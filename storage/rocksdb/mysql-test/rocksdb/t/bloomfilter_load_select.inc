# loading some data (larger than write buf size) to cause compaction
--exec perl suite/rocksdb/t/gen_insert.pl t1 >  $MYSQL_TMP_DIR/insert_t1.sql
--exec perl suite/rocksdb/t/gen_insert.pl t2 >  $MYSQL_TMP_DIR/insert_t2.sql
--disable_query_log
--source $MYSQL_TMP_DIR/insert_t1.sql
--source $MYSQL_TMP_DIR/insert_t2.sql
--enable_query_log

# BF conditions (prefix short(4B)|medium(20B)|long(240B))
#0 no eq condition (o, x, x)
## cond length 4, key length > 4
call bloom_start();
select count(*) from t1;
call bloom_end();
call bloom_start();
select count(*) from t2;
call bloom_end();
call bloom_start();
select count(*) from t1 force index(PRIMARY) where id1 >= 1;
call bloom_end();
call bloom_start();
select count(*) from t1 force index(id2_id1) where id2 >= 1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index(id3_id4) where id3 >= '1';
call bloom_end();

#1 cond length == prefix length (o, o, x)
## cond length 4+8+8=20, key length > 20
call bloom_start();
select count(*) from t1 force index(id2_id1)  where id2=2 and id1=1;
call bloom_end();
call bloom_start();
select count(*) from t1 force index(id2_id1)  where id2=24 and id1=12;
call bloom_end();
call bloom_start();
select count(*) from t1 force index(id2_id1)  where id2=88 and id1=44;
call bloom_end();
call bloom_start();
select count(*) from t1 force index(id2_id1)  where id2=100 and id1=50;
call bloom_end();
call bloom_start();
select count(*) from t1 force index(id2_id1)  where id2=428 and id1=214;
call bloom_end();
## (cond_length == extended_key_length(4+8+4+4=20) == prefix_length)
call bloom_start();
select count(*) from t2 force index (id2_id4_id5) where id2=1 and id4=1 and id5=1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4_id5) where id2=23 and id4=115 and id5=115;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4_id5) where id2=500 and id4=2500 and id5=2500;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4_id5) where id2=601 and id4=3005 and id5=3005;
call bloom_end();

#2 cond length < actual key length  and cond_length < prefix length (o, x, x)
## for long prefix key, most cases falling into this category, unless all key colums are used.
## cond length 4+8=12, key length > 12
call bloom_start();
select count(*) from t2 force index (id2_id3) where id2=1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id3) where id2=23;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id3) where id2=345;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id3) where id2=456;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4) where id2=1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4) where id2=23;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4) where id2=345;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id4) where id2=456;
call bloom_end();

#3 both actual key length and cond length >= prefix length (o, o, o/x)
## cond length 4+8+9+8+4=33
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=1 and id3='1' and id1=1 order by id4;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=36 and id3='36' and id1=18 order by id4;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=124 and id3='124' and id1=62 order by id4;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=888 and id3='888' and id1=444 order by id4;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=124 and id3='124';
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id3) where id2=1 and id3='1' and id4=1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2_id3) where id2=12 and id3='12' and id4=60;
call bloom_end();
## 4+8+9=25
call bloom_start();
select count(*) from t1 force index (id2_id3) where id2=1 and id3='1';
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3) where id2=23 and id3='23';
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id3_id2) where id2=1 and id3='1';
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id3_id2) where id2=23 and id3='23';
call bloom_end();

#4 actual key length > prefix length and cond length < prefix length (o, x, x)
## cond length 4+8=12
call bloom_start();
select count(*) from t1 force index (PRIMARY) where id1=1;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (PRIMARY) where id1=12;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (PRIMARY) where id1=23;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (PRIMARY) where id1=100;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (PRIMARY) where id1=234;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=36;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2_id3_id1_id4) where id2=234;
call bloom_end();

#5 cond length == extended key length < prefix length (o, o, o)
call bloom_start();
select count(*) from t2 force index (id2) where id2=1 and id4=1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2) where id2=23 and id4=115;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2) where id2=500 and id4=2500;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id2) where id2=601 and id4=3005;
call bloom_end();
## 4+9+4=17
call bloom_start();
select count(*) from t2 force index (id3_id4) where id3='1' and id4=1;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id3_id4) where id3='12' and id4=60;
call bloom_end();

#6 cond length == non-extended key length < prefix length, actual key length > prefix length (o, x, x)
call bloom_start();
select count(*) from t1 force index (id2) where id2=1;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2) where id2=23;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2) where id2=345;
call bloom_end();
call bloom_start();
select count(*) from t1 force index (id2) where id2=456;
call bloom_end();
## 4+9+4=17
call bloom_start();
select count(*) from t2 force index (id3_id5) where id3='100' and id5=500;
call bloom_end();
call bloom_start();
select count(*) from t2 force index (id3_id5) where id3='240' and id5=1200;
call bloom_end();

