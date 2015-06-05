package My::Suite::Mroonga;

@ISA = qw(My::Suite);

return "No Mroonga engine" unless $ENV{HA_MROONGA_SO} or
                                  $::mysqld_variables{'mroonga'} eq "ON";
#
# RECOMPILE_FOR_EMBEDDED also means that a plugin
# cannot be dynamically loaded into embedded
return "Not run for embedded server" if $::opt_embedded_server and
                                        $ENV{HA_MROONGA_SO};

sub is_default { 1 }

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

