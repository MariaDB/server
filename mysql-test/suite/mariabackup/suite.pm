package My::Suite::MariaBackup;

@ISA = qw(My::Suite);
use My::Find;
use My::Platform;
use File::Basename;
use strict;

return "Not run for embedded server" if $::opt_embedded_server;

return "No mariabackup" unless $ENV{XTRABACKUP};

# BACKUP SERVER WITH 'pipe' resolves mariadb-backup-pipe in the
# PATH of the mariadbd process, which mtr starts from
# this environment, so it has to be on PATH before the
# server is started rather than at --exec time.
# POSIX only: have_mariabackup_wrapper.inc skips Windows,
# where cmd.exe
# has no "sh" and -x would key off the file extension.
unless (IS_WINDOWS || `sh -c "command -v mariadb-backup-pipe"`)
{
  my $dir = dirname($ENV{XTRABACKUP});
  $ENV{PATH}="$dir:$ENV{PATH}" if -x "$dir/mariadb-backup-pipe";
}

my $have_qpress = index(`qpress 2>&1`,"Compression") > 0;

sub skip_combinations {
  my %skip;
  $skip{'include/have_file_key_management.inc'} = 'needs file_key_management plugin'  unless $ENV{FILE_KEY_MANAGEMENT_SO};
  $skip{'compress_qpress.test'}= 'needs qpress executable in PATH' unless $have_qpress;
  %skip;
}

bless { };
