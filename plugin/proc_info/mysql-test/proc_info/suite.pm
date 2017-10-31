package My::Suite::Proc_info;

@ISA = qw(My::Suite);

return "No PROC_INFO plugin" unless
  $ENV{PROC_INFO_SO} or
  $::mysqld_variables{'proc-info'} eq "ON";

return "Not run for embedded server" if $::opt_embedded_server;

sub is_default { 1 }

bless { };
