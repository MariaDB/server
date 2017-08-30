package My::Suite::MariaBackup;

@ISA = qw(My::Suite);
use My::Find;
use File::Basename;
use strict;

return "Not run for embedded server" if $::opt_embedded_server;

my $mariabackup_exe=
::mtr_exe_maybe_exists(
  "$::bindir/extra/mariabackup$::opt_vs_config/mariabackup",
  "$::path_client_bindir/mariabackup");

return "No mariabackup" if !$mariabackup_exe;


$ENV{XTRABACKUP}= $mariabackup_exe;

$ENV{XBSTREAM}= ::mtr_exe_maybe_exists(
      "$::bindir/extra/mariabackup/$::opt_vs_config/mbstream",
      "$::path_client_bindir/mbstream");

$ENV{INNOBACKUPEX}= "$mariabackup_exe --innobackupex";

sub skip_combinations {
  my %skip;
  $skip{'include/have_file_key_management.inc'} = 'needs file_key_management plugin'  unless $ENV{FILE_KEY_MANAGEMENT_SO};
  %skip;
}

bless { };

