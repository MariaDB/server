package My::Suite::MariaBackup;

@ISA = qw(My::Suite);
use My::Find;
use File::Basename;
use strict;

return "Not run for embedded server" if $::opt_embedded_server;

return "No mariabackup" unless $ENV{XTRABACKUP};

my $have_qpress = index(`qpress 2>&1`,"Compression") > 0;

sub skip_combinations {
  my %skip;
  $skip{'include/have_file_key_management.inc'} = 'needs file_key_management plugin'  unless $ENV{FILE_KEY_MANAGEMENT_SO};
  $skip{'compress_qpress.test'}= 'needs qpress executable in PATH' unless $have_qpress;
  %skip;
}

bless { };
