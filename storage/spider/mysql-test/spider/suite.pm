package My::Suite::Spider;

@ISA = qw(My::Suite);

return "No Spider engine" unless $ENV{HA_SPIDER_SO};

bless { };

