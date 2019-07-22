package My::Suite::WSREP_INFO;
use File::Basename;
use My::Find;

@ISA = qw(My::Suite);

return "Not run for embedded server" if $::opt_embedded_server;

return "WSREP is not compiled in" if not ::have_wsrep();

return "No wsrep provider library" unless ::have_wsrep_provider();

return ::wsrep_version_message() unless ::check_wsrep_version();

return "No WSREP_INFO plugin" unless $ENV{WSREP_INFO_SO};

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
