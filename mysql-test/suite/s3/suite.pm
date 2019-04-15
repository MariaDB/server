package My::Suite::S3;

@ISA = qw(My::Suite);

return "Need S3 engine" unless $::mysqld_variables{'s3'} eq "ON";

bless { };

