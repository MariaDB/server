package My::Suite::Connect;

@ISA = qw(My::Suite);

return "No CONNECT engine" unless $ENV{HA_CONNECT_SO} or
                                  $::mysqld_variables{'connect'} eq "ON";

bless { };

