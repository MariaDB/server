if (`SELECT COUNT(*)=0 FROM INFORMATION_SCHEMA.GLOBAL_VARIABLES WHERE
    VARIABLE_NAME LIKE 'wsrep_provider' AND VARIABLE_VALUE NOT LIKE 'none'`)
{
  --skip Test requires wsrep provider library (libgalera_smm.so)
}

