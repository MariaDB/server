package My::Suite::Parquet;

@ISA = qw(My::Suite);

return "Need Parquet engine" unless $ENV{HA_PARQUET_SO} or
  $::mysqld_variables{'parquet'} eq "ON";

bless {};
