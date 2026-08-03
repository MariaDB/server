# The goal of including this file is to test with different
# lower_case_table_names modes (see include/lcase_names.combinations).
# Skip a combination unless the server actually ended up in the intended mode.

if ($MTR_COMBINATION_LCASE0)
{
  --source include/have_lowercase0.inc
}
if ($MTR_COMBINATION_LCASE1)
{
  --source include/have_lowercase1.inc
}
if ($MTR_COMBINATION_LCASE2)
{
  --source include/have_lowercase2.inc
}
