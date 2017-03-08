eval set session autocommit=$autocommit;
let $is_gaplock_target = `SELECT @@autocommit = 0 && '$select_lock' != '' && '$expect_gap_lock_errors' = 1`;

if ($is_gaplock_target)
{
# rnd_init
--error ER_UNKNOWN_ERROR
eval select * from gap1 limit 1 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 where value != 100 limit 1 $select_lock;
# index_read_map
--error ER_UNKNOWN_ERROR
eval select * from gap1 where id1=1 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 where id1=1 and id2= 1 $select_lock;
# read_range_first
--error ER_UNKNOWN_ERROR
eval select * from gap1 where id1=1 and id2= 1 and id3 != 1 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 where id1=1 and id2= 1 and id3
  between 1 and 3 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 where id1=1 and id2= 1 order by id3 asc
  limit 1 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 where id1=1 and id2= 1 order by id3 desc
  limit 1 $select_lock;
# index_first
--error ER_UNKNOWN_ERROR
eval select * from gap1 order by id1 asc limit 1 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 order by id1 asc, id2 asc, id3 asc limit 1 $select_lock;
# index_last
--error ER_UNKNOWN_ERROR
eval select * from gap1 order by id1 desc limit 1 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap1 order by id1 desc, id2 desc, id3 desc
  limit 1 $select_lock;
# secondary index lookup
--error ER_UNKNOWN_ERROR
eval select * from gap1 force index(i) where c1=1 $select_lock;
# unique index lookup, ensure no gap lock errors as this is effectively a
# single point select that does not lock ranges or gaps of keys
eval select * from gap3 force index(ui) where value=1 $select_lock;
# primary key lookup, ensure no gap lock errors as these are effectively
# single point selects that do not lock ranges or gaps of keys
eval select * from gap1 where id1=1 and id2=1 and id3=1 $select_lock;
eval select * from gap1 where id1=1 and id2=1 and id3 in (1, 2, 3) $select_lock;
eval select * from gap1 where id1=1 and id2=1 and id3=1 and value=1
  order by c1 $select_lock;
eval select * from gap3 where id=1 $select_lock;
eval select * from gap4 where id=1 $select_lock;
eval select * from gap4 where id in (1, 2, 3) $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap4 $select_lock;
--error ER_UNKNOWN_ERROR
eval select * from gap4 where id between 3 and 7 $select_lock;
}

if (!$is_gaplock_target)
{
eval select * from gap1 limit 1 $select_lock;
eval select * from gap1 where value != 100 limit 1 $select_lock;
eval select * from gap1 where id1=1 $select_lock;
eval select * from gap1 where id1=1 and id2= 1 $select_lock;
eval select * from gap1 where id1=1 and id2= 1 and id3 != 1 $select_lock;
eval select * from gap1 where id1=1 and id2= 1 and id3
  between 1 and 3 $select_lock;
eval select * from gap1 where id1=1 and id2= 1 order by id3 asc
  limit 1 $select_lock;
eval select * from gap1 where id1=1 and id2= 1 order by id3 desc
  limit 1 $select_lock;
eval select * from gap1 order by id1 asc limit 1 $select_lock;
eval select * from gap1 order by id1 asc, id2 asc, id3 asc limit 1 $select_lock;
eval select * from gap1 order by id1 desc limit 1 $select_lock;
eval select * from gap1 order by id1 desc, id2 desc, id3 desc
  limit 1 $select_lock;
eval select * from gap1 force index(i) where c1=1 $select_lock;
eval select * from gap3 force index(ui) where value=1 $select_lock;
eval select * from gap1 where id1=1 and id2=1 and id3=1 $select_lock;
eval select * from gap1 where id1=1 and id2=1 and id3 in (1, 2, 3) $select_lock;
eval select * from gap1 where id1=1 and id2=1 and id3=1 and value=1
  order by c1 $select_lock;
eval select * from gap3 where id=1 $select_lock;
eval select * from gap4 where id=1 $select_lock;
eval select * from gap4 where id in (1, 2, 3) $select_lock;
eval select * from gap4 $select_lock;
eval select * from gap4 where id between 3 and 7 $select_lock;
}
