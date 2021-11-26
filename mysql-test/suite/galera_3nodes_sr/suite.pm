package My::Suite::GALERA_3NODES_SR;

use lib 'suite';
use wsrep::common;

@ISA = qw(My::Suite);

return wsrep_not_ok() if wsrep_not_ok();

push @::global_suppressions,
  (
     qr(WSREP: wsrep_sst_receive_address is set to '127.0.0.1),
     qr(WSREP: Could not open saved state file for reading: .*),
     qr(WSREP: Could not open state file for reading: .*),
     qr(WSREP: Gap in state sequence. Need state transfer.),
     qr(WSREP: Failed to prepare for incremental state transfer:),
     qr(WSREP:.*down context.*),
     qr(WSREP: Failed to send state UUID:),
     qr(WSREP: last inactive check more than .* skipping check),
     qr(WSREP: SQL statement was ineffective),
     qr(WSREP: Releasing seqno [0-9]* before [0-9]* was assigned.),
     qr|WSREP: access file\(.*gvwstate.dat\) failed\(No such file or directory\)|,
     qr(WSREP: Quorum: No node with complete state),
     qr(WSREP: Initial position was provided by configuration or SST, avoiding override),
     qr|WSREP: discarding established \(time wait\) .*|,
     qr(WSREP: There are no nodes in the same segment that will ever be able to become donors, yet there is a suitable donor outside. Will use that one.),
     qr(WSREP: evs::proto.*),
     qr|WSREP: Ignoring possible split-brain \(allowed by configuration\) from view:.*|,
     qr(WSREP: no nodes coming from prim view, prim not possible),
     qr(WSREP: Member .* requested state transfer from .* but it is impossible to select State Transfer donor: Resource temporarily unavailable),
     qr(WSREP: user message in state LEAVING),
     qr(WSREP: .* sending install message failed: Transport endpoint is not connected),
     qr(WSREP: .* sending install message failed: Resource temporarily unavailable),
     qr(WSREP: Sending JOIN failed: -107 \(Transport endpoint is not connected\). Will retry in new primary component.),
     qr(WSREP: Could not find peer:),
     qr|WSREP: gcs_caused\(\) returned .*|,
     qr|WSREP: Protocol violation. JOIN message sender .* is not in state transfer \(SYNCED\). Message ignored.|,
     qr|WSREP: Protocol violation. JOIN message sender .* is not in state transfer \(JOINED\). Message ignored.|,
     qr(WSREP: Action message in non-primary configuration from member [0-9]*),
     qr(WSREP: Last Applied Action message in non-primary configuration from member [0-9]*),
     qr|WSREP: .*core_handle_uuid_msg.*|,
     qr(WSREP: --wsrep-causal-reads=ON takes precedence over --wsrep-sync-wait=0. WSREP_SYNC_WAIT_BEFORE_READ is on),
     qr(WSREP: JOIN message from member .* in non-primary configuration. Ignored.),
   );

bless { };
