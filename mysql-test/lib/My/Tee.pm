package My::Tee;

# see PerlIO::via

our $copyfh;

sub PUSHED
{
  open($copyfh, '>', "$::opt_vardir/log/stdout.log")
    or die "open(>$::opt_vardir/log/stdout.log): $!"
      unless $copyfh;
 bless { }, shift;
}

sub WRITE
{
 my ($obj, $buf, $fh) = @_;
 print $fh $buf;
 print $copyfh $buf;
 return length($buf);
}

1;
