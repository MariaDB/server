eval create or replace table t1 (
 id1 bigint not null,
 id2 bigint not null,
 id3 varchar(100) not null,
 id4 int not null,
 id5 int not null,
 value bigint,
 value2 varchar(100),
 primary key (id1, id2, id3, id4) $CF,
 index id2 (id2) $CF,
 index id2_id1 (id2, id1) $CF,
 index id2_id3 (id2, id3) $CF,
 index id2_id4 (id2, id4) $CF,
 index id2_id3_id1_id4 (id2, id3, id1, id4) $CF,
 index id3_id2 (id3, id2) $CF
) engine=ROCKSDB;

eval create or replace table t2 (
 id1 bigint not null,
 id2 bigint not null,
 id3 varchar(100) not null,
 id4 int not null,
 id5 int not null,
 value bigint,
 value2 varchar(100),
 primary key (id4) $CF,
 index id2 (id2) $CF,
 index id2_id3 (id2, id3) $CF,
 index id2_id4 (id2, id4) $CF,
 index id2_id4_id5 (id2, id4, id5) $CF,
 index id3_id4 (id3, id4) $CF,
 index id3_id5 (id3, id5) $CF
) engine=ROCKSDB;
