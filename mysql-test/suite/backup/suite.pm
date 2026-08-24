package My::Suite::Backup;

@ISA = qw(My::Suite);
use My::Find;
use File::Basename;
use strict;

return "Not run for embedded server" if $::opt_embedded_server;

my $have_cat = index(`echo meowl|cat 2>&1`,"meowl") >= 0;
my $have_tar = `tar --version 2>&1` =~ /tar .*\d\.\d/;

sub skip_combinations {
  my %skip;
  $skip{'backup_stream.test'} = 'needs cat,tar' unless $have_cat && $have_tar;
  %skip;
}

bless { };
