package My::Suite::Encryption;

@ISA = qw(My::Suite);

sub skip_combinations {
  my @combinations;

  $skip{'include/have_file_key_management_plugin.combinations'} = [ 'ctr' ]
    unless $::mysqld_variables{'version-ssl-library'} =~ /OpenSSL (\S+)/
       and $1 ge "1.0.1";

  %skip;
}

bless { };

