package My::Suite::CTest;
use Cwd;

@ISA = qw(My::Suite);

sub list_cases {
  my ($self) = @_;
  keys %{$self->{ctests}}
}

sub start_test {
  my ($self, $tinfo)= @_;
  my $args;
  my $path;
  my $cmd = $self->{ctests}->{$tinfo->{shortname}};

  if ($cmd =~ /[ "'><%!*?]/) {
    ($path, $args) = ('/bin/sh', [ '-c', $cmd ])
  } else {
    ($path, $args) = ($cmd, , [ ])
  }


  my $oldpwd=getcwd();
  chdir $::opt_vardir;
  my $proc=My::SafeProcess->new
           (
            name          => $tinfo->{shortname},
            path          => $path,
            args          => \$args,
            append        => 1,
            output        => $::path_current_testlog,
            error         => $::path_current_testlog,
           );
  chdir $oldpwd;
  $proc;
}

{ 
  my $bin=$ENV{MTR_BINDIR} || '..';
  return "Not run for embedded server" if $::opt_embedded_server;
  return "Not configured to run ctest" unless -f "$bin/CTestTestfile.cmake";
  my ($ctest_vs)= $::multiconfig ? "-C ".substr($::multiconfig,1) : "";
  my (@ctest_list)= `cd "$bin" && ctest $ctest_vs --show-only --verbose`;
  return "No ctest" if $?;

  my ($command, %tests, $prefix);
  for (@ctest_list) {
    chomp;
    if (/^\d+: Test command: +/) {
      $command= $';
      $prefix= /libmariadb/ ? 'conc_' : '';
    } elsif (/^ +Test +#\d+: +/) {
      if ($command ne "NOT_AVAILABLE") {
        $tests{$prefix.$'}=$command;
      }
    }
  }
  bless { ctests => { %tests } };
}
