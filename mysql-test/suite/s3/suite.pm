package My::Suite::S3;

use Socket;

@ISA = qw(My::Suite);

return "Need S3 engine" unless $::mysqld_variables{'s3'} eq "ON" or $ENV{HA_S3_SO};

if (!$ENV{'S3_REGION'})
{
  $ENV{'S3_REGION'} = "";
}

if (!$ENV{'S3_ACCESS_KEY'})
{
  $ENV{'S3_ACCESS_KEY'} = "minio";
}

if (!$ENV{'S3_SECRET_KEY'})
{
  $ENV{'S3_SECRET_KEY'} = "minioadmin";
}

if (!$ENV{'S3_PROTOCOL_VERSION'})
{
  $ENV{'S3_PROTOCOL_VERSION'} = "Auto";
}

if (!$ENV{'S3_PORT'})
{
  $ENV{'S3_PORT'} = 0;
}

if (!$ENV{'S3_SSL_NO_VERIFY'})
{
  $ENV{'S3_SSL_NO_VERIFY'} = "OFF";
}

if (!$ENV{'S3_PROVIDER'})
{
  $ENV{'S3_PROVIDER'} = "Default";
}

if (!$ENV{'S3_HOST_NAME'})
{
  $ENV{'S3_HOST_NAME'} = "127.0.0.1";

  if (!$ENV{'S3_PORT'})
  {
    $ENV{'S3_PORT'} = 9000;
  }
  if (!$ENV{'S3_USE_HTTP'})
  {
    $ENV{'S3_USE_HTTP'} = "ON";
  }
}

if (!$ENV{'S3_USE_HTTP'})
{
  $ENV{'S3_USE_HTTP'} = "OFF";
}

if (!$ENV{'S3_BUCKET'})
{
  $ENV{'S3_BUCKET'} = "MariaDB";
}

bless { };
