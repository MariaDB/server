package My::Suite::GenEmbedding;

use My::SafeProcess;
use My::File::Path;
use mtr_report;

@ISA = qw(My::Suite);

sub openai_start {
    my ($openai, $test) = @_; # My::Config::Group, My::Test

    my $python_path="python3"; # TODO: Maybe use a config option, could also be "python" or a specific path
    my $file_path=$openai->value('filename');
    my $http_code=$openai->value('http_code');
    my $python_server_path="$ENV{MTR_SUITE_DIR}/minimal_socket_server.py";
    # PORT = sys.argv[1]
    # FILENAME = sys.argv[2]
    # RETURN_CODE = sys.argv[3]
    my $args;
    &::mtr_init_args(\$args);
    &::mtr_add_arg($args, $python_server_path);
    &::mtr_add_arg($args, "$ENV{OPENAI_PORT}");
    &::mtr_add_arg($args, $file_path);
    &::mtr_add_arg($args, $http_code);

    my $flask_log= "openai.log";
    $openai->{'proc'}= My::SafeProcess->new
    (
     name         => 'myname-openai',
     path         => $python_path,
     args         => \$args,
     output       => $flask_log,
     error        => $flask_log,
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