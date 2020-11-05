package My::Suite::AuthGSSAPI;

@ISA = qw(My::Suite);

return "No AUTH_GSSAPI plugin" unless $ENV{AUTH_GSSAPI_SO};

return "Not run for embedded server" if $::opt_embedded_server;

# Following environment variables may need to be set
if ($^O eq "MSWin32")
{
  chomp(my $whoami =`whoami /UPN 2>NUL` || `whoami`);
  my $fullname = $whoami;
  $fullname =~ s/\\/\\\\/; # SQL escaping for backslash
  $ENV{'GSSAPI_FULLNAME'}  = $fullname;
  $ENV{'GSSAPI_SHORTNAME'} = $ENV{'USERNAME'};
  chomp(my $sid = `powershell -Command "([System.Security.Principal.WindowsIdentity]::GetCurrent()).User.Value"`);
  $ENV{'SID'} = $sid;

}
else
{
  if (!$ENV{'GSSAPI_FULLNAME'})
  {
    my $s = `klist 2>/dev/null |grep 'Default principal: '`;
    if ($s)
    {
      chomp($s);
      my $fullname = substr($s,19);
      $ENV{'GSSAPI_FULLNAME'} = $fullname;
    }
  }
  $ENV{'GSSAPI_SHORTNAME'} = (split /@/, $ENV{'GSSAPI_FULLNAME'}) [0];
}


if (!$ENV{'GSSAPI_FULLNAME'}  || !$ENV{'GSSAPI_SHORTNAME'})
{
  return "Environment variable GSSAPI_SHORTNAME and GSSAPI_FULLNAME need to be set"
}

if ($::opt_verbose)
{
  foreach $var ('GSSAPI_SHORTNAME','GSSAPI_FULLNAME','GSSAPI_KEYTAB_PATH','GSSAPI_PRINCIPAL_NAME')
  {
    print "$var=$ENV{$var}\n";
  }
}
sub is_default { 1 }

bless { };

