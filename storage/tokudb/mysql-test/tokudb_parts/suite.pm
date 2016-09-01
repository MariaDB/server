package My::Suite::TokuDB_bugs;
use File::Basename;
@ISA = qw(My::Suite);

# Ensure we can run the TokuDB tests even if hugepages are enabled
$ENV{TOKU_HUGE_PAGES_OK}=1;

#return "Not run for embedded server" if $::opt_embedded_server;
return "No TokuDB engine" unless $ENV{HA_TOKUDB_SO} or $::mysqld_variables{tokudb};

sub is_default { not $::opt_embedded_server }

sub skip_combinations {
  my %skip = ();

  $skip{'t/partition_alter4_tokudb.test'} = "Requires test case timeout >= 20mins (currently $::opt_testcase_timeout)" if $::opt_testcase_timeout < 20;
  %skip;
}

bless { };

