package My::Suite::Plugins;

use My::Platform;

@ISA = qw(My::Suite);

if (-d '../sql' && !&::using_extern()) {
  my $src = "$::bindir/plugin/auth_pam/auth_pam_tool";
  my $dst = "$::plugindir/auth_pam_tool_dir/auth_pam_tool";
  ::mkpath( "$::plugindir/auth_pam_tool_dir");
  eval { symlink $src, $dst } or ::copy $src, $dst;
}

sub skip_combinations {
  my %skip;
  $skip{'t/pam_init.inc'} = 'No pam setup for mtr'
             unless -e '/etc/pam.d/mariadb_mtr';
  $skip{'t/pam_init.inc'} = 'Not run as user owning auth_pam_tool_dir'
             unless -o $::plugindir . '/auth_pam_tool_dir';
  %skip;
}

bless { };

