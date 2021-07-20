#! /usr/bin/perl

# Copyright (C) 2003,2008 MySQL AB
# Copyright (C) 2010,2017 Sergei Golubchik and MariaDB Corporation
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1335 USA

# Run gcov and report test coverage on only those code lines touched by
# a given list of commits.

use strict;
use warnings;

use Getopt::Long;
use File::Find;
use Cwd qw/realpath/;

my $opt_verbose=0;
my $opt_generate;
my $opt_help;
my $opt_purge;
my $opt_only_gcov;
my $opt_skip_gcov;

my %cov;
my $file_no=0;

GetOptions
  ("v|verbose+"    => \$opt_verbose,
   "h|help"        => \$opt_help,
   "p|purge"       => \$opt_purge,
   "g|generate"    => \$opt_generate,
   "o|only-gcov"   => \$opt_only_gcov,
   "s|skip-gcov"   => \$opt_skip_gcov,
  ) or usage();

usage() if $opt_help;

sub logv(@)     { print STDERR @_,"\n" if $opt_verbose; }
sub gcov_prefix($) { defined($_[0]) ? $_[0] || '#####' : '-' }

my $root= `git rev-parse --show-toplevel`;
chomp $root;

die "Failed to find tree root" unless $root;
$root=realpath($root).'/';
logv "Chdir $root";
chdir $root or die "chdir($root): $!";

my $res;
my $cmd;
if ($opt_purge)
{
  $cmd= "find . -name '*.da' -o -name '*.gcda' -o -name '*.gcov' -o ".
               "-name '*.dgcov' | grep -v 'README\.gcov' | xargs rm -f ''";
  logv "Running: $cmd";
  system($cmd)==0 or die "system($cmd): $? $!";
  exit 0;
}

find(\&gcov_one_file, $root);
find(\&write_coverage, $root) if $opt_generate;
exit 0 if $opt_only_gcov;

if (@ARGV) {
  print_gcov_for_diff(@ARGV);
} else {
  print_gcov_for_diff('HEAD') or print_gcov_for_diff('HEAD^');
}
exit 0;

sub print_gcov_for_diff {
  $cmd="git diff --no-prefix --ignore-space-change @_";
  logv "Running: $cmd";
  open PIPE, '-|', $cmd or die "Failed to popen '$cmd': $!: $?";
  my ($lnum, $cnt, $fcov, $acc, $printme, $fname);
  while (<PIPE>) {
    if (/^diff --git (.*) \1\n/) {
      print $acc if $printme;
      $fname=$1;
      $acc="dgcov $fname";
      $acc=('*' x length($acc)) . "\n$acc\n" . ('*' x length($acc));
      $lnum=undef;
      $fcov=$cov{realpath($fname)};
      $printme=0;
      logv "File: $fname";
      next;
    }
    if (/^@@ -\d+,\d+ \+(\d+),(\d+) @@/ and $fcov) {
      $lnum=$1;
      $cnt=$2;
      $acc.="\n@@ +$lnum,$cnt @\@$'";
      logv "  lines: $lnum,",$lnum+$cnt;
      next;
    }
    next unless $lnum and $cnt;
    $acc.=sprintf '%9s:%5s:%s', '', $lnum, $' if /^ /;
    ++$printme, $acc.=sprintf '%9s:%5s:%s', gcov_prefix($fcov->{$lnum}), $lnum, $' if /^\+/;
    die "$_^^^ dying", unless /^[- +]/;
    ++$lnum;
    --$cnt;
  }
  print $acc if $printme;
  close PIPE or die "command '$cmd' failed: $!: $?";
  return defined($fname);
}

sub usage {
  print <<END;
Usage: $0 --help
       $0 [options] [git diff arguments]

The dgcov program runs gcov for code coverage analysis, and reports missing
coverage only for those lines that are changed by the specified commit(s).
Commits are specified in the format of git diff arguments. For example:
 * All unpushed commits:        $0 \@{u} HEAD
 * All uncommitted changes:     $0 HEAD
 * Specific commit:             $0 <commit>^ <commit>

If no arguments are specified, it prints the coverage for all uncommitted
changes, if any, otherwise for the last commit.

Options:

  -h    --help        This help.
  -v    --verbose     Show commands run.
  -p    --purge       Delete all test coverage information, to prepare for a
                      new coverage test.
  -o    --only-gcov   Stop after running gcov, don't run git
  -s    --skip-gcov   Do not run gcov, assume .gcov files are already in place
  -g    --generate    Create .dgcov files for all source files

Prior to running this tool, MariaDB should be built with

  cmake -DENABLE_GCOV=ON

and the testsuite should be run. dgcov will report the coverage
for all lines modified in the specified commits.
END

  exit 1;
}

sub gcov_one_file {
  return unless /\.gcda$/;
  unless ($opt_skip_gcov) {
    $cmd= "gcov -il '$_' 2>/dev/null >/dev/null";
    print STDERR ++$file_no,"\r" if not $opt_verbose and -t STDERR;
    logv "Running: $cmd";
    system($cmd)==0 or die "system($cmd): $? $!";
  }

  # now, read the generated file
  for my $gcov_file (<$_*.gcov>) {
    open FH, '<', "$gcov_file" or die "open(<$gcov_file): $!";
    my $fname;
    while (<FH>) {
      chomp;
      if (/^function:/) {
        next;
      }
      if (/^file:/) {
        $fname=realpath(-f $' ? $' : $root.$');
        next;
      }
      next if /^lcount:\d+,-\d+/; # whatever that means
      unless (/^lcount:(\d+),(\d+)/ and $fname) {
        warn "unknown line '$_' in $gcov_file";
        next;
      }
      $cov{$fname}->{$1}+=$2;
    }
    close(FH);
  }
}

sub write_coverage {
  my $fn=$File::Find::name;
  my $h=$cov{$fn};
  return unless $h and $root eq substr $fn, 0, length($root);
  open I, '<', $fn or die "open(<$fn): $!";
  open O, '>', "$fn.dgcov" or die "open(>$fn.dgcov): $!";
  logv "Annotating: ", substr $fn, length($root);
  while (<I>) {
    printf O '%9s:%5s:%s', gcov_prefix($h->{$.}), $., $_;
  }
  close I;
  close O;
}
