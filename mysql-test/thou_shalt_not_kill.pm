package thou_shalt_not_kill;
require Exporter;
@ISA = 'Exporter';
@EXPORT_OK = 'kill';

use subs 'kill';
use Carp qw(cluck);

sub import {
  my $pkg = shift;
  $pkg->export('CORE::GLOBAL', 'kill', @_);
}

sub kill {
  return CORE::kill(@_) unless $_[0];
  cluck "kill(@_)";
  print "\e[1;31m" if -t STDOUT;
  system "pstree -c $_" foreach @_[1..$#_];
  print "\e[0;39m" if -t STDOUT;
  print STDERR 'Kill [y/n] ? ';
  my $answer=<STDIN>;
  return CORE::kill(@_) if $answer =~ /y/i or $answer eq "\n";
  1;
}

1;
