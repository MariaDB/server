# -*- cperl -*-

use strict;
use warnings;
use Cwd qw(abs_path);
use File::Path qw(make_path);
use FindBin;
use Test::More;

my $test_dir= abs_path("$FindBin::Bin/../..");
my $build_dir= $ENV{MTR_BINDIR};
plan skip_all => "Set MTR_BINDIR to a built MariaDB directory"
  unless defined $build_dir;
BAIL_OUT("MTR_BINDIR is not a directory: $build_dir") unless -d $build_dir;

$build_dir= abs_path($build_dir);
$ENV{MTR_BINDIR}= $build_dir;
my $evidence_dir= "$build_dir/mdev21279-evidence";
make_path($evidence_dir);

for my $case (
  ['parallel-repeat', 4, 2, 2, '', 0, 4],
  ['uneven-repeat', 5, 3, 3, '', 0, 5],
  ['serial-repeat', 3, 1, 1, '', 0, 3],
  ['no-repeat', 1, 2, 1, '', 0, 1],
  ['auto-repeat', 4, 'auto', 2, '', 0, 4],
  ['retry-repeat', 4, 2, 2, 'once', 1, 6],
  ['retry-serial', 2, 1, 1, 'once', 1, 3],
  ['failed-repeat', 4, 2, 2, 'always', 1, 4],
  ['failure-limit', 4, 1, 1, 'always', 1, 1]
)
{
  my ($label, $repeat, $parallel, $worker_count, $failure,
      $expected_exit, $executions)= @$case;
  next if @ARGV && !grep { $_ eq $label } @ARGV;
  local $ENV{MTR_MAX_PARALLEL}= 2;
  local $ENV{NUMBER_OF_PROCESSORS}= 2;
  local $ENV{MTR_PARALLEL_REPEAT_FAILURE}= $failure;
  my @command= (
    $^X, "$test_dir/mariadb-test-run.pl",
    "--suite=mtr", "--parallel=$parallel", "--repeat=$repeat",
    '--force', '--retry=' . ($failure eq 'once' ? 2 : 0),
    '--max-test-fail=' . ($label eq 'failure-limit' ? 1 : 0),
    '--max-save-datadir=0', '--max-save-core=0',
    "--vardir=$evidence_dir/var-$label",
    '--mysqld=--innodb-buffer-pool-size=32M',
    '--mysqld=--aria-pagecache-buffer-size=16M',
    '--mysqld=--innodb-log-file-size=16M',
    '--mysqld=--performance-schema=OFF',
    'mtr.parallel_repeat'
  );

  open(my $run, '-|', @command) or BAIL_OUT("Cannot start MTR: $!");
  my $output= do { local $/; <$run> };
  close($run);
  my $exit_status= $? >> 8;

  open(my $log, '>', "$evidence_dir/$label.log")
    or BAIL_OUT("Cannot write evidence: $!");
  print $log $output;
  close($log);

  is($exit_status, $expected_exit, "$label: MTR exit status");
  if ($parallel eq 'auto')
  {
    my ($selected)= $output =~ /Using parallel: (\d+)/;
    if ($^O eq 'MSWin32')
    {
      is($selected, 2, "$label: repeated tests raise the automatic worker cap");
    }
    else
    {
      ok(defined $selected && $selected >= 1 && $selected <= 2,
         "$label: automatic selection respects the worker cap");
    }
    $worker_count= $selected if defined $selected;
  }
  my (@workers, @results);
  while ($output =~ /^mtr\.parallel_repeat\s+(?:w(\d+)\s+)?\[\s*(?:\d+\s+)?((?:retry-)?(?:pass|fail))\s*\]/mg)
  {
    push @workers, $1 || 1;
    push @results, $2;
  }
  is(scalar @results, $executions, "$label: exact execution count");
  my %workers= map { $_ => 1 } @workers;
  is(scalar keys %workers, $worker_count, "$label: distinct workers");
  if (!$failure)
  {
    is(scalar(grep { $_ eq 'pass' } @results), $repeat,
       "$label: every repetition passes");
    like($output, qr/Completed: All $repeat tests were successful\./,
         "$label: exact summary total");
  }
  else
  {
    is(scalar(grep { $_ eq 'fail' } @results), $failure eq 'once' ? $worker_count : $executions,
       "$label: expected initial failures");
    is(scalar(grep { $_ eq 'retry-pass' } @results),
       $failure eq 'once' ? $worker_count : 0, "$label: retries are isolated per batch");
    like($output, qr/Failed \d+\/$executions tests,/,
         "$label: exact failure summary total");
    like($output, qr/Too many tests\(1\) failed!/, "$label: failure limit stops repeats")
      if $label eq 'failure-limit';
  }
  diag("$label workers: " . join(', ', sort keys %workers));
}
done_testing();
