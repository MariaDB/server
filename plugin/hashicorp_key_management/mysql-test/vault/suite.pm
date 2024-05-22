package My::Suite::Vault;
use My::Platform;

@ISA = qw(My::Suite);

use strict;

return "Hashicorp Key Management plugin tests are currently not available on Windows"
  if IS_WINDOWS;

return "You need to set the value of the VAULT_ADDR variable"
  unless $ENV{VAULT_ADDR};

return "You need to set the value of the VAULT_TOKEN variable"
  unless $ENV{VAULT_TOKEN};

return "Hashicorp Vault key management utility not found"
  unless `sh -c "command -v vault"`;

bless {};
