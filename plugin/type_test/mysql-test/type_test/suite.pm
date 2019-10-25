package My::Suite::Type_test;

@ISA = qw(My::Suite);

return "No TYPE_TEST plugin" unless $ENV{TYPE_TEST_SO};

sub is_default { 1 }

bless { };

