# include/galera_backup_lock_bug.inc
#
# A write committed while node 1 held a backup lock. Set $bug to 1 if the
# lock must block it:
# - FLUSH and BLOCK_DDL block writes to non-transactional tables,
# - BLOCK_DDL blocks DDL,
# - BLOCK_COMMIT blocks commits; InnoDB commits are not checked,
# - FTWRL blocks all writes.
# Set $bug_lock to the name of the lock.
#
# Parameters:
#   $bug_stage  START, FLUSH, BLOCK_DDL, BLOCK_COMMIT or FTWRL
#   $bug_op     name of the write
#

--let $bug= 0
--let $bug_lock= BACKUP STAGE $bug_stage
--let $bug_nontrans= 0
if ($bug_op == aria_nontrans)
{
  --let $bug_nontrans= 1
}
if ($bug_op == myisam)
{
  --let $bug_nontrans= 1
}
if ($bug_op == toi_aria_nontrans)
{
  --let $bug_nontrans= 1
}
if ($bug_op == toi_myisam)
{
  --let $bug_nontrans= 1
}
if ($bug_op == innodb+myisam)
{
  --let $bug_nontrans= 1
}
if ($bug_stage == FLUSH)
{
  --let $bug= $bug_nontrans
}
if ($bug_stage == BLOCK_DDL)
{
  --let $bug= $bug_nontrans
  if ($bug_op == toi_ddl)
  {
    --let $bug= 1
  }
}
if ($bug_stage == BLOCK_COMMIT)
{
  if ($bug_op != innodb)
  {
    --let $bug= 1
  }
}
if ($bug_stage == FTWRL)
{
  --let $bug= 1
  --let $bug_lock= FLUSH TABLES WITH READ LOCK
}
