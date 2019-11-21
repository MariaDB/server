package My::Suite::MariaBackup;

@ISA = qw(My::Suite);
use My::Find;
use File::Basename;
use strict;

return "Not run for embedded server" if $::opt_embedded_server;

return "No mariabackup" unless ::have_mariabackup();

my $have_qpress = index(`qpress 2>&1`,"Compression") > 0;

sub skip_combinations {
  my %skip;
  $skip{'compress_qpress.test'}= 'needs qpress executable in PATH' unless $have_qpress;
  %skip;
}

bless { };
