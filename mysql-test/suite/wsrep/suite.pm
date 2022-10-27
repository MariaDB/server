package My::Suite::WSREP;

use lib 'suite';
use wsrep::common;

@ISA = qw(My::Suite);

return wsrep_not_ok() if wsrep_not_ok();

push @::global_suppressions,
  (
     qr(WSREP: Could not open saved state file for reading: .*),
     qr(WSREP: Could not open state file for reading: .*),
     qr|WSREP: access file\(.*gvwstate.dat\) failed\(No such file or directory\)|,
   );

bless { };
