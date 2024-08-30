if (!`SELECT count(*) FROM information_schema.engines WHERE
      (support = 'YES' OR support = 'DEFAULT') AND
      engine = 's3'`)
{
  skip Need s3 engine;
}
if (`select @@global.s3_secret_key = "" or @@global.s3_access_key = ""`)
{
  skip S3 engine not configured;
}
