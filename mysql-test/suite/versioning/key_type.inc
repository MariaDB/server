--disable_query_log
if ($MTR_COMBINATION_UNIQUE)
{
    let $KEY_TYPE= unique;
}
if ($MTR_COMBINATION_PK)
{
    let $KEY_TYPE= primary key;
}
if ($MTR_COMBINATION_SEC)
{
    let $KEY_TYPE= key;
}
--enable_query_log
