package My::Suite::Connect;

use File::Basename;
use My::SafeProcess;

@ISA = qw(My::Suite);

return "No CONNECT engine" unless $ENV{HA_CONNECT_SO} or
                                  $::mysqld_variables{'connect'} eq "ON";

# RECOMPILE_FOR_EMBEDDED also means that a plugin
# cannot be dynamically loaded into embedded
return "Not run for embedded server" if $::opt_embedded_server and
                                        $ENV{HA_CONNECT_SO};

my $tiny_http= dirname(__FILE__) . '/tiny-http.pl';

sub tiny_http_start {
  my ($http, $test) = @_; # My::Config::Group, My::Test

  return if $http->{proc}; # Already started

  my $pidfile= $http->value('pid-file');
  unlink $pidfile;

  my $args;
  &::mtr_init_args(\$args);
  &::mtr_add_arg($args, $tiny_http);
  &::mtr_add_arg($args, $http->value('port'));
  &::mtr_add_arg($args, $http->value('file'));
  &::mtr_add_arg($args, $pidfile);

  my $log= "$::opt_vardir/log/" . $http->name() . ".log";
  $http->{proc}= My::SafeProcess->new
    (
     name         => $http->name(),
     path         => $^X,
     args         => \$args,
     output       => $log,
     error        => $log,
     append       => 1,
     nocore       => 1,
    );
  &::mtr_verbose("Started $http->{proc}");
}

sub tiny_http_wait {
  my ($http) = @_; # My::Config::Group

  return !&::sleep_until_file_created($http->value('pid-file'), '',
                                      $::opt_start_timeout, $http->{proc}, 60);
}

sub servers {
  ( qr/^tinyhttp$/ => {
      SORT => 400,
      START => \&tiny_http_start,
      WAIT => \&tiny_http_wait,
    }
  )
}

sub is_default { 1 }

bless { };
