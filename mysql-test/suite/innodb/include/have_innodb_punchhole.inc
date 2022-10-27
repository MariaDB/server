if (!`SELECT COUNT(*) FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE LOWER(variable_name) = 'innodb_have_punch_hole' AND variable_value = 'ON'`)
{
  --skip Test requires InnoDB compiled with fallocate(FALLOC_PUNCH_HOLE| FALLOC_KEEP_SIZE)
}
