package My::Suite::GenEmbedding;

use My::SafeProcess;
use My::File::Path;
use mtr_report;

@ISA = qw(My::Suite);

sub skip_combinations {
  my %skip;
  my $api_key_file = "$ENV{HOME}/openai_api_key.inc";
  $skip{'t/openai_server.test'}="No OpenAI API key" unless -r $api_key_file;
  $skip{'t/openai_greek_charset.test'}="No OpenAI API key" unless -r $api_key_file;
  %skip;
}

sub openai_start {
    my ($openai, $test) = @_; # My::Config::Group, My::Test
    if ($openai->{proc}) {
      # Already started
      return ;
    }
    my $python_path="python3"; # TODO: Maybe use a config option, could also be "python" or a specific path
    my $success_file_path=$openai->value('api_response_filename');
    my $python_server_path="$ENV{MTR_SUITE_DIR}/minimal_socket_server.py";
    my $args;
    &::mtr_init_args(\$args);
    &::mtr_add_arg($args, $python_server_path);
    &::mtr_add_arg($args, "$ENV{OPENAI_PORT}");
    &::mtr_add_arg($args, $success_file_path);
    # This can be changed to include the PORT or other info, perhaps in combination with append => 0
    $http_server_log= "openai.log"; 

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
    my ($openai, $test) = @_; # My::Config::Group, My::Test
    open(LOG, '<', $http_server_log);
    do {
      sleep 1 and seek LOG,0,1 until $_=<LOG>;
    } until /Started mockup API server on PORT:$ENV{OPENAI_PORT} .../;
    return 0;
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