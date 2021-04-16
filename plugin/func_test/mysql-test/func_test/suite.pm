package My::Suite::Func_test;

@ISA = qw(My::Suite);

return "No FUNC_TEST plugin" unless $ENV{FUNC_TEST_SO};

sub is_default { 1 }

bless { };
