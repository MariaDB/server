package My::Suite::BinlogEncryption;

@ISA = qw(My::Suite);

return "No file key management plugin" unless defined $ENV{FILE_KEY_MANAGEMENT_SO};

sub skip_combinations {
  my @combinations;

  $skip{'encryption_algorithms.combinations'} = [ 'ctr' ]
    unless $::mysqld_variables{'version-ssl-library'} =~ /OpenSSL (\S+)/
       and $1 ge "1.0.1";

  %skip;
}

bless { };

