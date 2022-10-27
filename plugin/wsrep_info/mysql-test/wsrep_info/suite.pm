package My::Suite::WSREP_INFO;
use File::Basename;
use My::Find;

@ISA = qw(My::Suite);

use lib 'suite';
use wsrep::common;
return wsrep_not_ok() if wsrep_not_ok();

push @::global_suppressions,
  (
     qr(WSREP:.*down context.*),
     qr(WSREP: Failed to send state UUID:.*),
     qr(WSREP: wsrep_sst_receive_address.*),
     qr(WSREP: Could not open saved state file for reading: .*),
     qr(WSREP: Could not open state file for reading: .*),
     qr(WSREP: last inactive check more than .* skipping check),
     qr(WSREP: Gap in state sequence. Need state transfer.),
     qr(WSREP: Failed to prepare for incremental state transfer: .*),
     qr(WSREP: SYNC message from member .* in non-primary configuration. Ignored.),
     qr|WSREP: access file\(.*gvwstate.dat\) failed\(No such file or directory\)|,
   );

sub is_default { 1 }

bless { };
