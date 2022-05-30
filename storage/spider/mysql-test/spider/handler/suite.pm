package My::Suite::Spider;

@ISA = qw(My::Suite);

return "No Spider engine" unless $ENV{HA_SPIDER_SO};
return "Not run for embedded server" if $::opt_embedded_server;

sub is_default { 1 }

bless { };
