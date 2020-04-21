package wsrep::common;

use base qw(Exporter);
our @EXPORT= qw(wsrep_not_ok);

use File::Basename;
use Memoize;
memoize 'wrong_wsrep_version';
memoize 'check_garbd_support';
memoize 'check_wsrep_support';
memoize 'wsrep_not_ok';

use mtr_report;

my $extra_path;
my $mariabackup_path;
my $mariabackup_exe;
my $garbd_exe;
my $file_wsrep_provider;

sub wrong_wsrep_version() {
  my $check_version= dirname($My::SafeProcess::safe_process_cmd[0]) . '/wsrep_check_version';
  my $checked = `$check_version -p`;
  chomp($checked);
  return $? ? $checked : undef;
}

sub which($) { return `sh -c "command -v $_[0]"` }

sub check_garbd_support() {
  my $wsrep_path= dirname($file_wsrep_provider);
  $garbd_exe= ::mtr_file_exists($wsrep_path."/garb/garbd",
                                $wsrep_path."/../../bin/garb/garbd",
                                '/usr/bin/garbd');
  $ENV{MTR_GARBD_EXE}= $garbd_exe if $garbd_exe;
}

sub check_wsrep_support() {
  mtr_report(" - binaries built with wsrep patch");

  # ADD scripts to $PATH to that wsrep_sst_* can be found
  my ($spath) = grep { -f "$_/wsrep_sst_rsync"; } "$::bindir/scripts", $::path_client_bindir;
  mtr_error("No SST scripts") unless $spath;
  $ENV{PATH}="$spath:$ENV{PATH}";

  # ADD mysql client library path to path so that wsrep_notify_cmd can find mysql
  # client for loading the tables. (Don't assume each machine has mysql install)
  my ($cpath) = grep { -f "$_/mysql"; } "$::bindir/scripts", $::path_client_bindir;
  mtr_error("No scritps") unless $cpath;
  $ENV{PATH}="$cpath:$ENV{PATH}" unless $cpath eq $spath;

  # ADD my_print_defaults script path to path so that SST scripts can find it
  my $my_print_defaults_exe=
    ::mtr_exe_maybe_exists(
      "$::bindir/extra/my_print_defaults",
      "$::path_client_bindir/my_print_defaults");
  my $epath= "";
  if ($my_print_defaults_exe ne "") {
     $epath= dirname($my_print_defaults_exe);
  }
  mtr_error("No my_print_defaults") unless $epath;
  $ENV{PATH}="$epath:$ENV{PATH}" unless ($epath eq $spath) or
                                                 ($epath eq $cpath);

  $extra_path= $epath;

  if (which("socat")) {
    $ENV{MTR_GALERA_TFMT}="socat";
  } elsif (which("nc")) {
    $ENV{MTR_GALERA_TFMT}="nc";
  }

  $ENV{PATH}=dirname($ENV{XTRABACKUP}).":$ENV{PATH}" if $ENV{XTRABACKUP};

  # Check whether WSREP_PROVIDER environment variable is set.
  if (defined $ENV{'WSREP_PROVIDER'}) {
    $file_wsrep_provider= "";
    if ($ENV{'WSREP_PROVIDER'} ne "none") {
      if (::mtr_file_exists($ENV{'WSREP_PROVIDER'}) ne "") {
        $file_wsrep_provider= $ENV{'WSREP_PROVIDER'};
      } else {
        mtr_error("WSREP_PROVIDER env set to an invalid path");
      }
      check_garbd_support();
    }
    # WSREP_PROVIDER is valid; set to a valid path or "none").
    mtr_verbose("WSREP_PROVIDER env set to $ENV{'WSREP_PROVIDER'}");
  } else {
    # WSREP_PROVIDER env not defined. Lets try to locate the wsrep provider
    # library.
    $file_wsrep_provider=
      ::mtr_file_exists("/usr/lib64/galera-4/libgalera_smm.so",
                      "/usr/lib64/galera/libgalera_smm.so",
                      "/usr/lib/galera-4/libgalera_smm.so",
                      "/usr/lib/galera/libgalera_smm.so");
    if ($file_wsrep_provider ne "") {
      # wsrep provider library found !
      mtr_verbose("wsrep provider library found : $file_wsrep_provider");
      $ENV{'WSREP_PROVIDER'}= $file_wsrep_provider;
      check_garbd_support();
    } else {
      mtr_verbose("Could not find wsrep provider library, setting it to 'none'");
      $ENV{'WSREP_PROVIDER'}= "none";
    }
  }
}

sub wsrep_not_ok() {
  return "Not run for embedded server" if $::opt_embedded_server;
  return "WSREP is not compiled in" if not $::mysqld_variables{'wsrep-on'};
  check_wsrep_support();
  return "No wsrep provider library" unless $file_wsrep_provider;
  return wrong_wsrep_version() if wrong_wsrep_version();
  undef;
}

1;
