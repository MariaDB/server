package My::Suite::Videx;

@ISA = qw(My::Suite);

return "No VIDEX" unless $ENV{HA_VIDEX_SO} or
                           $::mysqld_variables{'videx'} eq "ON";

return "Not run for embedded server" if $::opt_embedded_server;

sub is_default { 1 }

bless { };
