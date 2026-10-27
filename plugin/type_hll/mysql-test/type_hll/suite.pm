package My::Suite::Type_hll;

@ISA = qw(My::Suite);

return "No TYPE_HLL plugin" unless $ENV{TYPE_HLL_SO};

sub is_default { 1 }

bless { };

