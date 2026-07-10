package My::Suite::AuthSHA2;
@ISA = qw(My::Suite);
return "Not run for embedded server" if $::opt_embedded_server;
sub is_default { 1 }
bless { };
