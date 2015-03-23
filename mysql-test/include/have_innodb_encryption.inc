#
# Ensure we have innodb encryption incompiled

if (`select count(*)=0 from information_schema.global_variables where variable_name="innodb_data_encryption_providertype"`)
{
  --skip Test requires InnoDB encryption.
}
