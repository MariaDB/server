package My::Suite::Sql_extension;

@ISA = qw(My::Suite);

return "No TEST_SQL_EXTENSION plugin" unless $ENV{TEST_SQL_EXTENSION_SO};

sub is_default { 1 }

bless { };
