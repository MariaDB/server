package My::Suite::HandlerSocket;

@ISA = qw(My::Suite);

return "No handlersocket plugin" unless $ENV{HANDLERSOCKET_SO};

sub is_default { not  $::opt_embedded_server }

bless { };
