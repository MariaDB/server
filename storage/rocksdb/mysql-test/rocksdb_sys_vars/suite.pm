package My::Suite::Rocksdb_sys_vars;

@ISA = qw(My::Suite);

sub is_default { not $::opt_embedded_server }

bless { };

