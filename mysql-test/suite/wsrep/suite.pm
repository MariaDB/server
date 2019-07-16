package My::Suite::WSREP;
use File::Basename;
use My::Find;

@ISA = qw(My::Suite);

return "Not run for embedded server" if $::opt_embedded_server;

return "WSREP is not compiled in" unless ::have_wsrep();

return "No wsrep provider library" unless ::have_wsrep_provider();

return ::wsrep_version_message() unless ::check_wsrep_version();

push @::global_suppressions,
  (
     qr(WSREP: Could not open saved state file for reading: .*),
     qr(WSREP: Could not open state file for reading: .*),
     qr|WSREP: access file\(.*gvwstate.dat\) failed\(No such file or directory\)|,
   );

bless { };
