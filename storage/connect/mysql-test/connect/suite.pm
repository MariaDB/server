package My::Suite::Connect;

@ISA = qw(My::Suite);

return "No CONNECT engine" unless $ENV{HA_CONNECT_SO} or
                                  $::mysqld_variables{'connect'} eq "ON";

# RECOMPILE_FOR_EMBEDDED also means that a plugin
# cannot be dynamically loaded into embedded
return "Not run for embedded server" if $::opt_embedded_server and
                                        $ENV{HA_CONNECT_SO};

sub is_default { 1 }                                  

bless { };

