package My::Suite::Type_inet;

@ISA = qw(My::Suite);

#return "No inet6 plugin" unless $::mysqld_variables{'inet6'} eq "ON";

sub is_default { 1 }

bless { };
