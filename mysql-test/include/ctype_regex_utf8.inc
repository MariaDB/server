--echo #
--echo # Bug #3928 regexp [[:>:]] and UTF-8
--echo #
SELECT @@character_set_client, @@collation_connection;

--echo # This should return TRUE
select  'вася'  rlike '\\bвася\\b';
select  'вася ' rlike '\\bвася\\b';
select ' вася'  rlike '\\bвася\\b';
select ' вася ' rlike '\\bвася\\b';

select  'вася'  rlike '[[:<:]]вася[[:>:]]';
select  'вася ' rlike '[[:<:]]вася[[:>:]]';
select ' вася'  rlike '[[:<:]]вася[[:>:]]';
select ' вася ' rlike '[[:<:]]вася[[:>:]]';

--echo # This should return FALSE
select  'васяz' rlike '\\bвася\\b';
select 'zвася'  rlike '\\bвася\\b';
select 'zвасяz' rlike '\\bвася\\b';

select  'васяz' rlike '[[:<:]]вася[[:>:]]';
select 'zвася'  rlike '[[:<:]]вася[[:>:]]';
select 'zвасяz' rlike '[[:<:]]вася[[:>:]]';
