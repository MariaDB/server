/*
  Unit test: the unlock-time table verification must not run unless this
  handle both holds a lock and has changed the table under it.

  ha_heap::external_lock(F_UNLCK) verifies the table with heap_check_heap().
  That is safe on the ordinary unlock path, which sql/lock.cc describes as
  external_lock(F_UNLCK) followed by thr_multi_unlock(), so the lock is still
  held.  It is not safe on either path that unlocks after a failed lock
  attempt: when mysql_lock_tables() balances the external locks it took
  because thr_multi_lock() timed out, and when lock_external() itself unwinds
  the tables it already locked because a later table refused -- sql/lock.cc
  spells out the second of those, the first is only in its code.  In both the
  caller holds nothing, while another connection is writing.

  The requested lock type cannot tell those apart on its own: external_lock()
  has already recorded it on both, before thr_multi_lock() runs.  What tells
  them apart is that neither ever ran a row operation, so this handle has
  changed nothing since the lock was recorded.  HP_INFO::changed carries that,
  and it is per handle -- HP_SHARE::changed answers a different question,
  "did anyone change this table", which is true on exactly the paths that
  must be suppressed.

  Every outcome below is forced rather than raced: the states are built
  directly, in the order ha_heap::external_lock() builds them.
*/

#include "hp_test_helpers.h"

/*
  Reproduce what ha_heap::external_lock() does when a lock is granted: record
  the type and start a fresh change epoch.  lock_external() reaches this for
  every table it locks, including the ones it is about to unwind.
*/

static void grant_lock(HP_INFO *info, int lock_type)
{
  info->lock_type= lock_type;
  info->changed= 0;
}


/*
  Reproduce the state heap_write() is in between allocating a slot and marking
  it visible: next_free_record_pos() has already published the slot in
  total_records, but the record has not been stored yet.

  The slot is zeroed rather than left as it comes from my_malloc() so that the
  test asserts on a defined outcome.  A zero flags byte is what a scan of a
  half-written row legitimately sees; leaving the malloc garbage in place is
  what makes the same access an uninitialised read under MSAN.
*/

static uchar *park_mid_write(HP_SHARE *share)
{
  uchar *pos= next_free_record_pos(share);
  if (pos)
    memset(pos, 0, share->block.recbuffer);
  return pos;
}


static void unpark_mid_write(HP_SHARE *share, uchar *pos)
{
  hp_push_free_record(share, pos);
  hp_shrink_tail(share);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  HP_SHARE *share;
  HP_INFO *info1, *info2;
  uchar rec[REC_LENGTH], rec2[REC_LENGTH];
  uchar blob_data[200];
  uchar *parked_slot;
  int i;

  MY_INIT("hp_test_unlock_check-t");
  plan(15);

  if (create_and_open("test_unlock_check", &share, &info1))
  {
    ok(0, "setup failed");
    my_end(0);
    return exit_status();
  }

  /*
    A second handle on the same share, which is what a second connection
    holds.  It never runs a row operation in this test.
  */
  if (!(info2= heap_open("test_unlock_check", 2)))
  {
    ok(0, "second open failed");
    heap_drop_table(info1);
    my_end(0);
    return exit_status();
  }
  heap_extra(info2, HA_EXTRA_NO_READCHECK);

  for (i= 0; i < (int) sizeof(blob_data); i++)
    blob_data[i]= (uchar) ('a' + (i % 26));

  ok(heap_check_heap(info1, 0) == 0, "table is consistent before the test");

  /* --- the three row operations each open a change epoch --- */

  grant_lock(info1, F_WRLCK);
  build_record(rec, 1, blob_data, (uint16) sizeof(blob_data));
  if (heap_write(info1, rec))
  {
    ok(0, "heap_write failed");
    goto cleanup;
  }
  ok(info1->changed, "heap_write marks the writing handle as having changed");
  ok(share->changed, "heap_write marks the share as changed too");

  grant_lock(info1, F_WRLCK);
  build_record(rec2, 1, blob_data, (uint16) (sizeof(blob_data) / 2));
  if (heap_scan_init(info1) || heap_scan(info1, rec) ||
      heap_update(info1, rec, rec2))
  {
    ok(0, "heap_update failed");
    goto cleanup;
  }
  ok(info1->changed, "heap_update marks the updating handle as having changed");

  grant_lock(info1, F_WRLCK);
  if (heap_scan_init(info1) || heap_scan(info1, rec) ||
      heap_delete(info1, rec))
  {
    ok(0, "heap_delete failed");
    goto cleanup;
  }
  ok(info1->changed, "heap_delete marks the deleting handle as having changed");

  /*
    The record that separates this from HP_SHARE::changed.  info2 has run
    nothing, so it must report no change of its own even though the share it
    shares with info1 is changed -- that combination is precisely the failed
    lock attempt, where another connection is writing and this one is not.
  */
  grant_lock(info2, F_WRLCK);
  ok(share->changed && !info2->changed,
     "a handle that ran nothing is unchanged while the share is changed");

  /* --- the lock type half --- */

  info1->changed= 1;
  grant_lock(info1, F_UNLCK);
  info1->changed= 1;
  ok(!table_is_locked_and_changed(info1),
     "an unlocked handle claims nothing, whatever it changed before");

  grant_lock(info1, F_RDLCK);
  info1->changed= 1;
  ok(table_is_locked_and_changed(info1), "a read-locked handle claims a lock");

  grant_lock(info1, F_WRLCK);
  info1->changed= 1;
  ok(table_is_locked_and_changed(info1), "a write-locked handle claims a lock");

  grant_lock(info1, F_EXTRA_LCK);
  info1->changed= 1;
  ok(table_is_locked_and_changed(info1),
     "a table private to its session counts as always locked");

  /* --- the change half, on a handle whose lock type is armed --- */

  grant_lock(info1, F_WRLCK);
  ok(!table_is_locked_and_changed(info1),
     "an armed but unused handle claims nothing: the failed lock attempt");

  /* --- the gate as a whole --- */

  info1->changed= 1;
  ok(hp_may_check_heap_on_unlock(info1),
     "locked, changed and healthy: the table may be verified");

  heap_mark_crashed(share);
  ok(!hp_may_check_heap_on_unlock(info1),
     "a table already marked crashed is not verified again");
  heap_clear_state(share);

  /*
    The payload.  Park the share in the state a writer passes through mid-row,
    so the verification has something to wrongly find, and confirm that the
    handle which did not write is the one being kept away from it.
  */
  parked_slot= park_mid_write(share);
  if (!parked_slot)
  {
    ok(0, "could not park the share mid-write");
    goto cleanup;
  }

  grant_lock(info2, F_WRLCK);
  ok(!hp_may_check_heap_on_unlock(info2) && heap_check_heap(info2, 0) != 0,
     "the check is suppressed on a handle whose scan would report damage");
  heap_clear_state(share);

  unpark_mid_write(share, parked_slot);
  ok(heap_check_heap(info1, 0) == 0,
     "the table was consistent all along: the report was a false positive");

cleanup:
  heap_close(info2);
  heap_drop_table(info1);
  my_end(0);
  return exit_status();
}
