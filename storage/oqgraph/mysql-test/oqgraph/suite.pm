package My::Suite::OQGraph;

@ISA = qw(My::Suite);

return "No OQGraph" unless $ENV{HA_OQGRAPH_SO} or
                           $::mysqld_variables{'oqgraph'} eq "ON";

# as long as OQGraph defines MYSQL_SERVER it cannot run in embedded
return "Not run for embedded server" if $::opt_embedded_server;

sub is_default { 1 }

bless { };

