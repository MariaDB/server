##
# $input the value of a boolean type
# $output the value of int type
##
--let $int_value=$value
if ($value==on)
{
  --let $int_value=1
}

if ($value==off)
{
  --let $int_value=0
}

# MySQL allows 'true' and 'false' on bool values
if ($value==true)
{
  --let $int_value=1
}

if ($value==false)
{
  --let $int_value=0
}
