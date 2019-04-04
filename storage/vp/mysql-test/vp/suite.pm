package My::Suite::Vp;

@ISA = qw(My::Suite);

return "No Vp engine" unless $ENV{HA_VP_SO};
return "Not run for embedded server" if $::opt_embedded_server;

sub is_default { 1 }

bless { };

