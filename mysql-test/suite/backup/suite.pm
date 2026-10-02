package My::Suite::Backup;
use My::Platform;

@ISA = qw(My::Suite);

use strict;

return "Not run for embedded server" if $::opt_embedded_server;

my $have_mbc = IS_WINDOWS || `sh -c "command -v mariadb-backup-cat"`;
unless ($have_mbc)
{
    my @path = grep { -x "$_/mariadb-backup-cat" } "$::path_client_bindir";
    $ENV{PATH}="$path[0]:$ENV{PATH}" if ($have_mbc = $#path >= 0);
}

my $have_cat = index(`echo meowl|cat 2>&1`,"meowl") >= 0;
my $have_tar = `tar --version 2>&1` =~ /tar .*\d\.\d/;

sub skip_combinations {
  my %skip;
  unless ($have_cat && $have_tar && $have_mbc)
  {
    my $needs= 'needs cat,tar,mariadb-backup-cat';
    $skip{'backup_stream.test'} = $needs;
    $skip{'backup_innodb_data_directory.test'} = $needs;
  }
  %skip;
}

bless { };
