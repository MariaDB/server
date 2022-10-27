package My::Suite::User_variables;

@ISA = qw(My::Suite);

return "No USER_VARIABLES plugin" unless
  $ENV{USER_VARIABLES_SO} or
  $::mysqld_variables{'user-variables'} eq "ON";

return "Not run for embedded server" if $::opt_embedded_server and
                                        $ENV{USER_VARIABLES_SO};

sub is_default { 1 }

bless { };
