package My::Suite::Memcached;
use File::Basename;

@ISA = qw(My::Suite);

$ENV{DAEMON_MEMCACHED_OPT}="--plugin_dir=$::plugindir";
$ENV{DAEMON_MEMCACHED_ENGINE_DIR}=$::plugindir;

bless { };

