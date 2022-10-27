package My::Suite::Mroonga;

@ISA = qw(My::Suite);

return "No Mroonga engine" unless $ENV{HA_MROONGA_SO} or
                                  $::mysqld_variables{'mroonga'} eq "ON";

sub is_default { not $::opt_embedded_server }

my $groonga_normalizer_mysql_dir=$::basedir . '/storage/mroonga/vendor/groonga/vendor/plugins/groonga-normalizer-mysql';
my $groonga_normalizer_mysql_install_dir=$::basedir . '/lib/groonga/plugins';

if (-d $groonga_normalizer_mysql_dir)
{
  $ENV{GRN_PLUGINS_DIR}=$groonga_normalizer_mysql_dir;
}
elsif (-d $groonga_normalizer_mysql_install_dir)
{
  $ENV{GRN_PLUGINS_DIR}=$groonga_normalizer_mysql_install_dir;
}

bless { };

