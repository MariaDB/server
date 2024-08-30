package My::Suite::S3;

use Socket;

@ISA = qw(My::Suite);

return "Need S3 engine" unless $::mysqld_variables{'s3'} eq "ON" or $ENV{HA_S3_SO};

my $paddr = sockaddr_in(9000, INADDR_ANY);
my $protocol = getprotobyname("tcp");
socket(SOCK, PF_INET, SOCK_STREAM, $protocol);

if(connect(SOCK, $paddr))
{
  $ENV{'S3_HOST_NAME'} = "127.0.0.1";
  $ENV{'S3_PORT'} = 9000;
  $ENV{'S3_BUCKET'} = "storage-engine";
  $ENV{'S3_ACCESS_KEY'} = "minio";
  $ENV{'S3_SECRET_KEY'} = "minioadmin";
  $ENV{'S3_REGION'} = "";
  $ENV{'S3_PROTOCOL_VERSION'} = "Auto";
  $ENV{'S3_USE_HTTP'} = "ON";
}
else
{
  if (!$ENV{'S3_HOST_NAME'})
  {
    $ENV{'S3_HOST_NAME'} = "s3.amazonaws.com";
  }

  if (!$ENV{'S3_BUCKET'})
  {
    $ENV{'S3_BUCKET'} = "MariaDB";
  }

  if (!$ENV{'S3_REGION'})
  {
    $ENV{'S3_REGION'} = "";
  }

  if (!$ENV{'S3_ACCESS_KEY'})
  {
    return "Environment variable S3_ACCESS_KEY need to be set";
  }

  if (!$ENV{'S3_SECRET_KEY'})
  {
    return "Environment variable S3_SECRET_KEY need to be set";
  }

  if (!$ENV{'S3_PROTOCOL_VERSION'})
  {
    $ENV{'S3_PROTOCOL_VERSION'} = "Auto";
  }

  if (!$ENV{'S3_PORT'})
  {
    $ENV{'S3_PORT'} = 0;
  }

  if (!$ENV{'S3_USE_HTTP'})
  {
    $ENV{'S3_USE_HTTP'} = "OFF";
  }
}
bless { };

