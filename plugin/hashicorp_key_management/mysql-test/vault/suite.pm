package My::Suite::Vault;

@ISA = qw(My::Suite);

use strict;

return "Hashicorp Vault key management utility not found"
  unless `sh -c "command -v vault"`;

return "You need to set the value of the VAULT_ADDR variable"
  unless $ENV{VAULT_ADDR};

return "You need to set the value of the VAULT_TOKEN variable"
  unless $ENV{VAULT_TOKEN};

bless {};
