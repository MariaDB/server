package My::Suite::GenEmbedding;

use My::SafeProcess;
use My::File::Path;
use mtr_report;

@ISA = qw(My::Suite);

sub skip_combinations {
  my %skip;
  my $api_key_file = "$ENV{HOME}/openai_api_key.inc";
  $skip{'t/openai_server.test'}="No OpenAI API key" unless -r $api_key_file;
  %skip;
}

sub openai_start {
    my ($openai, $test) = @_; # My::Config::Group, My::Test
    if ($openai->{proc}) {
      # Already started
      return ;
    }
    my $python_path="python3"; # TODO: Maybe use a config option, could also be "python" or a specific path
    my $success_file_path=$openai->value('success_filename');
    my $wrong_json_path_file_path=$openai->value('wrong_json_path_filename');
    my $python_server_path="$ENV{MTR_SUITE_DIR}/minimal_socket_server.py";
    my $args;
    &::mtr_init_args(\$args);
    &::mtr_add_arg($args, $python_server_path);
    &::mtr_add_arg($args, "$ENV{OPENAI_PORT}");
    &::mtr_add_arg($args, $success_file_path);
    &::mtr_add_arg($args, $wrong_json_path_file_path);
    my $http_server_log= "openai.log";

    $openai->{'proc'}= My::SafeProcess->new
    (
     name         => 'myname-openai',
     path         => $python_path,
     args         => \$args,
     output       => $http_server_log,
     error        => $http_server_log,
     append       => 1,
     nocore       => 1,
    );
}

sub openai_wait {
    my ($opneai, $test) = @_; # My::Config::Group, My::Test
    my $cmd= "echo Waiting for openai mockup server at port $ENV{OPENAI_PORT}"; 
    system $cmd;
}

sub servers {
  ( qr/^openai$/ => {
    SORT => 400,
    START => \&openai_start,
    WAIT => \&openai_wait,
    }
  )
}

bless { };