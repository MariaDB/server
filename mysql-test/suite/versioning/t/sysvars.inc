--disable_query_log
if ($MTR_COMBINATION_TRX_ID)
{
  set @s1= 1;
  set @e1= 1;
  set @s2= 3;
  set @e2= 2;
  set @s3= 4;
  set @e3= 5;
}
if (!$MTR_COMBINATION_TRX_ID)
{
  set @s1= '1998-01-01 00:00';
  set @e1= '1998-01-01 00:00';
  set @s2= '1999-01-01 01:00';
  set @e2= '1999-01-01 00:00';
  set @s3= '2000-01-01 00:00';
  set @e3= '2000-01-01 01:00';
}
--enable_query_log
