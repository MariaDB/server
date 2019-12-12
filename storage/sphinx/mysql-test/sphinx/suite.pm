package My::Suite::Sphinx;

use My::SafeProcess;
use My::File::Path;
use mtr_report;

@ISA = qw(My::Suite);

############# initialization ######################
sub locate_sphinx_binary {
  my ($name)= @_;
  my $res;
  my @list= map "$_/$name", split /:/, $ENV{PATH};
  my $env_override= $ENV{"SPHINXSEARCH_\U$name"};
  @list= ($env_override) if $env_override;
  for (@list) { return $_ if -x $_; }
}

# Look for Sphinx binaries
my $exe_sphinx_indexer = &locate_sphinx_binary('indexer');
return "'indexer' binary not found" unless $exe_sphinx_indexer;

my $exe_sphinx_searchd = &locate_sphinx_binary('searchd');
return "'searchd' binary not found" unless $exe_sphinx_searchd;

my $sphinx_config= "$::opt_vardir/my_sphinx.conf";

# Check for Sphinx engine

return "SphinxSE not found" unless $ENV{HA_SPHINX_SO} or $::mysqld_variables{'sphinx'} eq "ON";

{
  local $_ = `"$exe_sphinx_searchd" --help`;
  mtr_verbose("tool: $exe_sphinx_searchd\n$_");
  my $ver = sprintf "%04d.%04d.%04d", (/([0-9]+)\.([0-9]+)(?:\.([0-9]+))?/);
  return "Sphinx 2.0.4 or later is needed (found $ver) " unless $ver ge '0002.0000.0004';
}

############# action methods ######################

sub write_sphinx_conf {
  my ($config) = @_; # My::Config
  my $res;

  foreach my $group ($config->groups()) {
    my $name= $group->{name};
    # Only the ones relevant to Sphinx search.
    next unless ($name eq 'indexer' or $name eq 'searchd' or
                 $name =~ /^(source|index) \w+$/);
    $res .= "$name\n{\n";
    foreach my $option ($group->options()) {
      $res .= $option->name();
      my $value= $option->value();
      if (defined $value) {
	$res .= "=$value";
      }
      $res .= "\n";
    }
    $res .= "}\n\n";
  }
  $res;
}

sub searchd_start {
  my ($sphinx, $test) = @_; # My::Config::Group, My::Test

  return unless $exe_sphinx_indexer and $exe_sphinx_searchd;
  return if $sphinx->{proc}; # Already started

  # First we must run the indexer to create the data.
  my $sphinx_data_dir= "$::opt_vardir/" . $sphinx->name();
  mkpath($sphinx_data_dir);
  my $sphinx_log= $sphinx->value('#log-error');
  my $sphinx_config= "$::opt_vardir/my_sphinx.conf";
  my $cmd= "\"$exe_sphinx_indexer\" --config \"$sphinx_config\" test1 > \"$sphinx_log\" 2>&1";
  &::mtr_verbose("cmd: $cmd");
  system $cmd;

  # Then start the searchd daemon.
  my $args;
  &::mtr_init_args(\$args);
  &::mtr_add_arg($args, "--config");
  &::mtr_add_arg($args, $sphinx_config);
  &::mtr_add_arg($args, "--console");
  &::mtr_add_arg($args, "--pidfile");

  $sphinx->{'proc'}= My::SafeProcess->new
    (
     name         => 'sphinx-' . $sphinx->name(),
     path         => $exe_sphinx_searchd,
     args         => \$args,
     output       => $sphinx_log,
     error        => $sphinx_log,
     append       => 1,
     nocore       => 1,
    );
  &::mtr_verbose("Started $sphinx->{proc}");
}

sub wait_exp_backoff {
  my $timeout= shift; # Seconds
  my $start_wait= shift; # Seconds
  my $scale_factor= shift;

  $searchd_status= "$exe_sphinx_searchd --status" .
                   " --config $sphinx_config > /dev/null 2>&1";

  my $scale= $start_wait;
  my $total_sleep= 0;
  while (1) {
    my $status = system($searchd_status);
    if (not $status) {
      return 0;
    }
    if ($total_sleep >= $timeout) {
      last;
    }

    &::mtr_milli_sleep($scale * 1000);
    $total_sleep+= $scale;
    $scale*= $scale_factor;
  }

  &::mtr_warning("Getting a response from searchd timed out");
  return 1
}

sub searchd_wait {
  my ($sphinx) = @_; # My::Config::Group

  return wait_exp_backoff(30, 0.1, 2)
}

############# declaration methods ######################

sub config_files() {
  ( 'my_sphinx.conf' => \&write_sphinx_conf )
}

sub servers {
  ( qr/^searchd$/ => {
      SORT => 400,
      START => \&searchd_start,
      WAIT => \&searchd_wait,
    }
  )
}

sub is_default { 0 }

############# return an object ######################
bless { };

