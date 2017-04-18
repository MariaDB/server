use warnings FATAL => 'all';
use strict;
use English qw(-no_match_vars);
use POSIX "strftime";

my @required_perl_version = (5, 0, 5);
my $required_perl_version_old_style = 5.005;

# check existence of DBD::mysql module
eval {
    require DBD::mysql;
};
my $dbd_mysql_installed = $EVAL_ERROR ? 0 : 1;

my $now;
my %mysql;
my $prefix = "version_check";


# ###########################################################################
# HTTPMicro package
# This package is a copy without comments from the original.  The original
# with comments and its test file can be found in the Bazaar repository at,
#   lib/HTTPMicro.pm
#   t/lib/HTTPMicro.t
# See https://launchpad.net/percona-toolkit for more information.
# ###########################################################################
{

package HTTPMicro;
BEGIN {
  $HTTPMicro::VERSION = '0.001';
}
use strict;
use warnings;

use Carp ();


my @attributes;
BEGIN {
    @attributes = qw(agent timeout);
    no strict 'refs';
    for my $accessor ( @attributes ) {
        *{$accessor} = sub {
            @_ > 1 ? $_[0]->{$accessor} = $_[1] : $_[0]->{$accessor};
        };
    }
}

sub new {
    my($class, %args) = @_;
    (my $agent = $class) =~ s{::}{-}g;
    my $self = {
        agent        => $agent . "/" . ($class->VERSION || 0),
        timeout      => 60,
    };
    for my $key ( @attributes ) {
        $self->{$key} = $args{$key} if exists $args{$key}
    }
    return bless $self, $class;
}

my %DefaultPort = (
    http => 80,
    https => 443,
);

sub request {
    my ($self, $method, $url, $args) = @_;
    @_ == 3 || (@_ == 4 && ref $args eq 'HASH')
      or Carp::croak(q/Usage: $http->request(METHOD, URL, [HASHREF])/);
    $args ||= {}; # we keep some state in this during _request

    my $response;
    for ( 0 .. 1 ) {
        $response = eval { $self->_request($method, $url, $args) };
        last unless $@ && $method eq 'GET'
            && $@ =~ m{^(?:Socket closed|Unexpected end)};
    }

    if (my $e = "$@") {
        $response = {
            success => q{},
            status  => 599,
            reason  => 'Internal Exception',
            content => $e,
            headers => {
                'content-type'   => 'text/plain',
                'content-length' => length $e,
            }
        };
    }
    return $response;
}

sub _request {
    my ($self, $method, $url, $args) = @_;

    my ($scheme, $host, $port, $path_query) = $self->_split_url($url);

    my $request = {
        method    => $method,
        scheme    => $scheme,
        host_port => ($port == $DefaultPort{$scheme} ? $host : "$host:$port"),
        uri       => $path_query,
        headers   => {},
    };

    my $handle  = HTTPMicro::Handle->new(timeout => $self->{timeout});

    $handle->connect($scheme, $host, $port);

    $self->_prepare_headers_and_cb($request, $args);
    $handle->write_request_header(@{$request}{qw/method uri headers/});
    $handle->write_content_body($request) if $request->{content};

    my $response;
    do { $response = $handle->read_response_header }
        until (substr($response->{status},0,1) ne '1');

    if (!($method eq 'HEAD' || $response->{status} =~ /^[23]04/)) {
        $response->{content} = '';
        $handle->read_content_body(sub { $_[1]->{content} .= $_[0] }, $response);
    }

    $handle->close;
    $response->{success} = substr($response->{status},0,1) eq '2';
    return $response;
}

sub _prepare_headers_and_cb {
    my ($self, $request, $args) = @_;

    for ($args->{headers}) {
        next unless defined;
        while (my ($k, $v) = each %$_) {
            $request->{headers}{lc $k} = $v;
        }
    }
    $request->{headers}{'host'}         = $request->{host_port};
    $request->{headers}{'connection'}   = "close";
    $request->{headers}{'user-agent'} ||= $self->{agent};

    if (defined $args->{content}) {
        $request->{headers}{'content-type'} ||= "application/octet-stream";
        utf8::downgrade($args->{content}, 1)
            or Carp::croak(q/Wide character in request message body/);
        $request->{headers}{'content-length'} = length $args->{content};
        $request->{content} = $args->{content};
    }
    return;
}

sub _split_url {
    my $url = pop;

    my ($scheme, $authority, $path_query) = $url =~ m<\A([^:/?#]+)://([^/?#]*)([^#]*)>
      or Carp::croak(qq/Cannot parse URL: '$url'/);

    $scheme     = lc $scheme;
    $path_query = "/$path_query" unless $path_query =~ m<\A/>;

    my $host = (length($authority)) ? lc $authority : 'localhost';
       $host =~ s/\A[^@]*@//;   # userinfo
    my $port = do {
       $host =~ s/:([0-9]*)\z// && length $1
         ? $1
         : $DefaultPort{$scheme}
    };

    return ($scheme, $host, $port, $path_query);
}

package
    HTTPMicro::Handle; # hide from PAUSE/indexers
use strict;
use warnings;

use Carp       qw[croak];
use Errno      qw[EINTR EPIPE];
use IO::Socket qw[SOCK_STREAM];

sub BUFSIZE () { 32768 }

my $Printable = sub {
    local $_ = shift;
    s/\r/\\r/g;
    s/\n/\\n/g;
    s/\t/\\t/g;
    s/([^\x20-\x7E])/sprintf('\\x%.2X', ord($1))/ge;
    $_;
};

sub new {
    my ($class, %args) = @_;
    return bless {
        rbuf             => '',
        timeout          => 60,
        max_line_size    => 16384,
        %args
    }, $class;
}

my $ssl_verify_args = {
    check_cn => "when_only",
    wildcards_in_alt => "anywhere",
    wildcards_in_cn => "anywhere"
};

sub connect {
    @_ == 4 || croak(q/Usage: $handle->connect(scheme, host, port)/);
    my ($self, $scheme, $host, $port) = @_;

    if ( $scheme eq 'https' ) {
        eval "require IO::Socket::SSL"
            unless exists $INC{'IO/Socket/SSL.pm'};
        croak(qq/IO::Socket::SSL must be installed for https support\n/)
            unless $INC{'IO/Socket/SSL.pm'};
    }
    elsif ( $scheme ne 'http' ) {
      croak(qq/Unsupported URL scheme '$scheme'\n/);
    }

    $self->{fh} = 'IO::Socket::INET'->new(
        PeerHost  => $host,
        PeerPort  => $port,
        Proto     => 'tcp',
        Type      => SOCK_STREAM,
        Timeout   => $self->{timeout}
    ) or croak(qq/Could not connect to '$host:$port': $@/);

    binmode($self->{fh})
      or croak(qq/Could not binmode() socket: '$!'/);

    if ( $scheme eq 'https') {
        IO::Socket::SSL->start_SSL($self->{fh});
        ref($self->{fh}) eq 'IO::Socket::SSL'
            or die(qq/SSL connection failed for $host\n/);
        if ( $self->{fh}->can("verify_hostname") ) {
            $self->{fh}->verify_hostname( $host, $ssl_verify_args )
                or die(qq/SSL certificate not valid for $host\n/);
        }
        else {
         my $fh = $self->{fh};
         _verify_hostname_of_cert($host, _peer_certificate($fh), $ssl_verify_args)
               or die(qq/SSL certificate not valid for $host\n/);
         }
    }
      
    $self->{host} = $host;
    $self->{port} = $port;

    return $self;
}

sub close {
    @_ == 1 || croak(q/Usage: $handle->close()/);
    my ($self) = @_;
    CORE::close($self->{fh})
      or croak(qq/Could not close socket: '$!'/);
}

sub write {
    @_ == 2 || croak(q/Usage: $handle->write(buf)/);
    my ($self, $buf) = @_;

    my $len = length $buf;
    my $off = 0;

    local $SIG{PIPE} = 'IGNORE';

    while () {
        $self->can_write
          or croak(q/Timed out while waiting for socket to become ready for writing/);
        my $r = syswrite($self->{fh}, $buf, $len, $off);
        if (defined $r) {
            $len -= $r;
            $off += $r;
            last unless $len > 0;
        }
        elsif ($! == EPIPE) {
            croak(qq/Socket closed by remote server: $!/);
        }
        elsif ($! != EINTR) {
            croak(qq/Could not write to socket: '$!'/);
        }
    }
    return $off;
}

sub read {
    @_ == 2 || @_ == 3 || croak(q/Usage: $handle->read(len)/);
    my ($self, $len) = @_;

    my $buf  = '';
    my $got = length $self->{rbuf};

    if ($got) {
        my $take = ($got < $len) ? $got : $len;
        $buf  = substr($self->{rbuf}, 0, $take, '');
        $len -= $take;
    }

    while ($len > 0) {
        $self->can_read
          or croak(q/Timed out while waiting for socket to become ready for reading/);
        my $r = sysread($self->{fh}, $buf, $len, length $buf);
        if (defined $r) {
            last unless $r;
            $len -= $r;
        }
        elsif ($! != EINTR) {
            croak(qq/Could not read from socket: '$!'/);
        }
    }
    if ($len) {
        croak(q/Unexpected end of stream/);
    }
    return $buf;
}

sub readline {
    @_ == 1 || croak(q/Usage: $handle->readline()/);
    my ($self) = @_;

    while () {
        if ($self->{rbuf} =~ s/\A ([^\x0D\x0A]* \x0D?\x0A)//x) {
            return $1;
        }
        $self->can_read
          or croak(q/Timed out while waiting for socket to become ready for reading/);
        my $r = sysread($self->{fh}, $self->{rbuf}, BUFSIZE, length $self->{rbuf});
        if (defined $r) {
            last unless $r;
        }
        elsif ($! != EINTR) {
            croak(qq/Could not read from socket: '$!'/);
        }
    }
    croak(q/Unexpected end of stream while looking for line/);
}

sub read_header_lines {
    @_ == 1 || @_ == 2 || croak(q/Usage: $handle->read_header_lines([headers])/);
    my ($self, $headers) = @_;
    $headers ||= {};
    my $lines   = 0;
    my $val;

    while () {
         my $line = $self->readline;

         if ($line =~ /\A ([^\x00-\x1F\x7F:]+) : [\x09\x20]* ([^\x0D\x0A]*)/x) {
             my ($field_name) = lc $1;
             $val = \($headers->{$field_name} = $2);
         }
         elsif ($line =~ /\A [\x09\x20]+ ([^\x0D\x0A]*)/x) {
             $val
               or croak(q/Unexpected header continuation line/);
             next unless length $1;
             $$val .= ' ' if length $$val;
             $$val .= $1;
         }
         elsif ($line =~ /\A \x0D?\x0A \z/x) {
            last;
         }
         else {
            croak(q/Malformed header line: / . $Printable->($line));
         }
    }
    return $headers;
}

sub write_header_lines {
    (@_ == 2 && ref $_[1] eq 'HASH') || croak(q/Usage: $handle->write_header_lines(headers)/);
    my($self, $headers) = @_;

    my $buf = '';
    while (my ($k, $v) = each %$headers) {
        my $field_name = lc $k;
         $field_name =~ /\A [\x21\x23-\x27\x2A\x2B\x2D\x2E\x30-\x39\x41-\x5A\x5E-\x7A\x7C\x7E]+ \z/x
            or croak(q/Invalid HTTP header field name: / . $Printable->($field_name));
         $field_name =~ s/\b(\w)/\u$1/g;
         $buf .= "$field_name: $v\x0D\x0A";
    }
    $buf .= "\x0D\x0A";
    return $self->write($buf);
}

sub read_content_body {
    @_ == 3 || @_ == 4 || croak(q/Usage: $handle->read_content_body(callback, response, [read_length])/);
    my ($self, $cb, $response, $len) = @_;
    $len ||= $response->{headers}{'content-length'};

    croak("No content-length in the returned response, and this "
        . "UA doesn't implement chunking") unless defined $len;

    while ($len > 0) {
        my $read = ($len > BUFSIZE) ? BUFSIZE : $len;
        $cb->($self->read($read), $response);
        $len -= $read;
    }

    return;
}

sub write_content_body {
    @_ == 2 || croak(q/Usage: $handle->write_content_body(request)/);
    my ($self, $request) = @_;
    my ($len, $content_length) = (0, $request->{headers}{'content-length'});

    $len += $self->write($request->{content});

    $len == $content_length
      or croak(qq/Content-Length missmatch (got: $len expected: $content_length)/);

    return $len;
}

sub read_response_header {
    @_ == 1 || croak(q/Usage: $handle->read_response_header()/);
    my ($self) = @_;

    my $line = $self->readline;

    $line =~ /\A (HTTP\/(0*\d+\.0*\d+)) [\x09\x20]+ ([0-9]{3}) [\x09\x20]+ ([^\x0D\x0A]*) \x0D?\x0A/x
      or croak(q/Malformed Status-Line: / . $Printable->($line));

    my ($protocol, $version, $status, $reason) = ($1, $2, $3, $4);

    return {
        status   => $status,
        reason   => $reason,
        headers  => $self->read_header_lines,
        protocol => $protocol,
    };
}

sub write_request_header {
    @_ == 4 || croak(q/Usage: $handle->write_request_header(method, request_uri, headers)/);
    my ($self, $method, $request_uri, $headers) = @_;

    return $self->write("$method $request_uri HTTP/1.1\x0D\x0A")
         + $self->write_header_lines($headers);
}

sub _do_timeout {
    my ($self, $type, $timeout) = @_;
    $timeout = $self->{timeout}
        unless defined $timeout && $timeout >= 0;

    my $fd = fileno $self->{fh};
    defined $fd && $fd >= 0
      or croak(q/select(2): 'Bad file descriptor'/);

    my $initial = time;
    my $pending = $timeout;
    my $nfound;

    vec(my $fdset = '', $fd, 1) = 1;

    while () {
        $nfound = ($type eq 'read')
            ? select($fdset, undef, undef, $pending)
            : select(undef, $fdset, undef, $pending) ;
        if ($nfound == -1) {
            $! == EINTR
              or croak(qq/select(2): '$!'/);
            redo if !$timeout || ($pending = $timeout - (time - $initial)) > 0;
            $nfound = 0;
        }
        last;
    }
    $! = 0;
    return $nfound;
}

sub can_read {
    @_ == 1 || @_ == 2 || croak(q/Usage: $handle->can_read([timeout])/);
    my $self = shift;
    return $self->_do_timeout('read', @_)
}

sub can_write {
    @_ == 1 || @_ == 2 || croak(q/Usage: $handle->can_write([timeout])/);
    my $self = shift;
    return $self->_do_timeout('write', @_)
}

my $prog = <<'EOP';
BEGIN {
   if ( defined &IO::Socket::SSL::CAN_IPV6 ) {
      *CAN_IPV6 = \*IO::Socket::SSL::CAN_IPV6;
   }
   else {
      constant->import( CAN_IPV6 => '' );
   }
   my %const = (
      NID_CommonName => 13,
      GEN_DNS => 2,
      GEN_IPADD => 7,
   );
   while ( my ($name,$value) = each %const ) {
      no strict 'refs';
      *{$name} = UNIVERSAL::can( 'Net::SSLeay', $name ) || sub { $value };
   }
}
{
   my %dispatcher = (
      issuer =>  sub { Net::SSLeay::X509_NAME_oneline( Net::SSLeay::X509_get_issuer_name( shift )) },
      subject => sub { Net::SSLeay::X509_NAME_oneline( Net::SSLeay::X509_get_subject_name( shift )) },
   );
   if ( $Net::SSLeay::VERSION >= 1.30 ) {
      $dispatcher{commonName} = sub {
         my $cn = Net::SSLeay::X509_NAME_get_text_by_NID(
            Net::SSLeay::X509_get_subject_name( shift ), NID_CommonName);
         $cn =~s{\0$}{}; # work around Bug in Net::SSLeay <1.33
         $cn;
      }
   } else {
      $dispatcher{commonName} = sub {
         croak "you need at least Net::SSLeay version 1.30 for getting commonName"
      }
   }

   if ( $Net::SSLeay::VERSION >= 1.33 ) {
      $dispatcher{subjectAltNames} = sub { Net::SSLeay::X509_get_subjectAltNames( shift ) };
   } else {
      $dispatcher{subjectAltNames} = sub {
         return;
      };
   }

   $dispatcher{authority} = $dispatcher{issuer};
   $dispatcher{owner}     = $dispatcher{subject};
   $dispatcher{cn}        = $dispatcher{commonName};

   sub _peer_certificate {
      my ($self, $field) = @_;
      my $ssl = $self->_get_ssl_object or return;

      my $cert = ${*$self}{_SSL_certificate}
         ||= Net::SSLeay::get_peer_certificate($ssl)
         or return $self->error("Could not retrieve peer certificate");

      if ($field) {
         my $sub = $dispatcher{$field} or croak
            "invalid argument for peer_certificate, valid are: ".join( " ",keys %dispatcher ).
            "\nMaybe you need to upgrade your Net::SSLeay";
         return $sub->($cert);
      } else {
         return $cert
      }
   }


   my %scheme = (
      ldap => {
         wildcards_in_cn    => 0,
         wildcards_in_alt => 'leftmost',
         check_cn         => 'always',
      },
      http => {
         wildcards_in_cn    => 'anywhere',
         wildcards_in_alt => 'anywhere',
         check_cn         => 'when_only',
      },
      smtp => {
         wildcards_in_cn    => 0,
         wildcards_in_alt => 0,
         check_cn         => 'always'
      },
      none => {}, # do not check
   );

   $scheme{www}  = $scheme{http}; # alias
   $scheme{xmpp} = $scheme{http}; # rfc 3920
   $scheme{pop3} = $scheme{ldap}; # rfc 2595
   $scheme{imap} = $scheme{ldap}; # rfc 2595
   $scheme{acap} = $scheme{ldap}; # rfc 2595
   $scheme{nntp} = $scheme{ldap}; # rfc 4642
   $scheme{ftp}  = $scheme{http}; # rfc 4217


   sub _verify_hostname_of_cert {
      my $identity = shift;
      my $cert = shift;
      my $scheme = shift || 'none';
      if ( ! ref($scheme) ) {
         $scheme = $scheme{$scheme} or croak "scheme $scheme not defined";
      }

      return 1 if ! %$scheme; # 'none'

      my $commonName = $dispatcher{cn}->($cert);
      my @altNames   = $dispatcher{subjectAltNames}->($cert);

      if ( my $sub = $scheme->{callback} ) {
         return $sub->($identity,$commonName,@altNames);
      }


      my $ipn;
      if ( CAN_IPV6 and $identity =~m{:} ) {
         $ipn = IO::Socket::SSL::inet_pton(IO::Socket::SSL::AF_INET6,$identity)
            or croak "'$identity' is not IPv6, but neither IPv4 nor hostname";
      } elsif ( $identity =~m{^\d+\.\d+\.\d+\.\d+$} ) {
         $ipn = IO::Socket::SSL::inet_aton( $identity ) or croak "'$identity' is not IPv4, but neither IPv6 nor hostname";
      } else {
         if ( $identity =~m{[^a-zA-Z0-9_.\-]} ) {
            $identity =~m{\0} and croak("name '$identity' has \\0 byte");
            $identity = IO::Socket::SSL::idn_to_ascii($identity) or
               croak "Warning: Given name '$identity' could not be converted to IDNA!";
         }
      }

      my $check_name = sub {
         my ($name,$identity,$wtyp) = @_;
         $wtyp ||= '';
         my $pattern;
         if ( $wtyp eq 'anywhere' and $name =~m{^([a-zA-Z0-9_\-]*)\*(.+)} ) {
            $pattern = qr{^\Q$1\E[a-zA-Z0-9_\-]*\Q$2\E$}i;
         } elsif ( $wtyp eq 'leftmost' and $name =~m{^\*(\..+)$} ) {
            $pattern = qr{^[a-zA-Z0-9_\-]*\Q$1\E$}i;
         } else {
            $pattern = qr{^\Q$name\E$}i;
         }
         return $identity =~ $pattern;
      };

      my $alt_dnsNames = 0;
      while (@altNames) {
         my ($type, $name) = splice (@altNames, 0, 2);
         if ( $ipn and $type == GEN_IPADD ) {
            return 1 if $ipn eq $name;

         } elsif ( ! $ipn and $type == GEN_DNS ) {
            $name =~s/\s+$//; $name =~s/^\s+//;
            $alt_dnsNames++;
            $check_name->($name,$identity,$scheme->{wildcards_in_alt})
               and return 1;
         }
      }

      if ( ! $ipn and (
         $scheme->{check_cn} eq 'always' or
         $scheme->{check_cn} eq 'when_only' and !$alt_dnsNames)) {
         $check_name->($commonName,$identity,$scheme->{wildcards_in_cn})
            and return 1;
      }

      return 0; # no match
   }
}
EOP

eval { require IO::Socket::SSL };
if ( $INC{"IO/Socket/SSL.pm"} ) {
   eval $prog;
   die $@ if $@;
}

1;
}
# ###########################################################################
# End HTTPMicro package
# ###########################################################################

# ###########################################################################
# VersionCheck package
# This package is a copy without comments from the original.  The original
# with comments and its test file can be found in the Bazaar repository at,
#   lib/VersionCheck.pm
#   t/lib/VersionCheck.t
# See https://launchpad.net/percona-toolkit for more information.
# ###########################################################################
{
package VersionCheck;


use strict;
use warnings FATAL => 'all';
use English qw(-no_match_vars);

use constant PTDEBUG => $ENV{PTDEBUG} || 0;

use Data::Dumper;
local $Data::Dumper::Indent    = 1;
local $Data::Dumper::Sortkeys  = 1;
local $Data::Dumper::Quotekeys = 0;

use Digest::MD5 qw(md5_hex);
use Sys::Hostname qw(hostname);
use File::Basename qw();
use File::Spec;
use FindBin qw();

eval {
   require Percona::Toolkit;
   require HTTPMicro;
};

{
   my $file    = 'percona-version-check';
   my $home    = $ENV{HOME} || $ENV{HOMEPATH} || $ENV{USERPROFILE} || '.';
   my @vc_dirs = (
      '/etc/percona',
      '/etc/percona-toolkit',
      '/tmp',
      "$home",
   );

   if ($ENV{PTDEBUG_VERSION_CHECK_HOME}) {
       @vc_dirs = ( $ENV{PTDEBUG_VERSION_CHECK_HOME} );
   }

   sub version_check_file {
      foreach my $dir ( @vc_dirs ) {
         if ( -d $dir && -w $dir ) {
            PTDEBUG && _d('Version check file', $file, 'in', $dir);
            return $dir . '/' . $file;
         }
      }
      PTDEBUG && _d('Version check file', $file, 'in', $ENV{PWD});
      return $file;  # in the CWD
   } 
}

sub version_check_time_limit {
   return 60 * 60 * 24;  # one day
}


sub version_check {
   my (%args) = @_;

   my $instances = $args{instances} || [];
   my $instances_to_check;

   PTDEBUG && _d('FindBin::Bin:', $FindBin::Bin);
   if ( !$args{force} ) {
      if ( $FindBin::Bin
           && (-d "$FindBin::Bin/../.bzr" || -d "$FindBin::Bin/../../.bzr") ) {
         PTDEBUG && _d("$FindBin::Bin/../.bzr disables --version-check");
         return;
      }
   }

   eval {
      foreach my $instance ( @$instances ) {
         my ($name, $id) = get_instance_id($instance);
         $instance->{name} = $name;
         $instance->{id}   = $id;
      }

      push @$instances, { name => 'system', id => 0 };

      $instances_to_check = get_instances_to_check(
         instances => $instances,
         vc_file   => $args{vc_file},  # testing
         now       => $args{now},      # testing
      );
      PTDEBUG && _d(scalar @$instances_to_check, 'instances to check');
      return unless @$instances_to_check;

      my $protocol = 'https';
      eval { require IO::Socket::SSL; };
      if ( $EVAL_ERROR ) {
          PTDEBUG && _d($EVAL_ERROR);
          PTDEBUG && _d("SSL not available, won't run version_check");
          return;
      }
      PTDEBUG && _d('Using', $protocol);

      my $advice = pingback(
         instances => $instances_to_check,
         protocol  => $protocol,
         url       => $args{url}                       # testing
                   || $ENV{PERCONA_VERSION_CHECK_URL}  # testing
                   || "$protocol://v.percona.com",
      );
      if ( $advice ) {
         PTDEBUG && _d('Advice:', Dumper($advice));
         if ( scalar @$advice > 1) {
            print "\n# " . scalar @$advice . " software updates are "
               . "available:\n";
         }
         else {
            print "\n# A software update is available:\n";
         }
         print join("\n", map { "#   * $_" } @$advice), "\n\n";
      }
   };
   if ( $EVAL_ERROR ) {
      PTDEBUG && _d('Version check failed:', $EVAL_ERROR);
   }

   if ( @$instances_to_check ) {
      eval {
         update_check_times(
            instances => $instances_to_check,
            vc_file   => $args{vc_file},  # testing
            now       => $args{now},      # testing
         );
      };
      if ( $EVAL_ERROR ) {
         PTDEBUG && _d('Error updating version check file:', $EVAL_ERROR);
      }
   }

   if ( $ENV{PTDEBUG_VERSION_CHECK} ) {
      warn "Exiting because the PTDEBUG_VERSION_CHECK "
         . "environment variable is defined.\n";
      exit 255;
   }

   return;
}

sub get_instances_to_check {
   my (%args) = @_;

   my $instances = $args{instances};
   my $now       = $args{now}     || int(time);
   my $vc_file   = $args{vc_file} || version_check_file();

   if ( !-f $vc_file ) {
      PTDEBUG && _d('Version check file', $vc_file, 'does not exist;',
         'version checking all instances');
      return $instances;
   }

   open my $fh, '<', $vc_file or die "Cannot open $vc_file: $OS_ERROR";
   chomp(my $file_contents = do { local $/ = undef; <$fh> });
   PTDEBUG && _d('Version check file', $vc_file, 'contents:', $file_contents);
   close $fh;
   my %last_check_time_for = $file_contents =~ /^([^,]+),(.+)$/mg;

   my $check_time_limit = version_check_time_limit();
   my @instances_to_check;
   foreach my $instance ( @$instances ) {
      my $last_check_time = $last_check_time_for{ $instance->{id} };
      PTDEBUG && _d('Intsance', $instance->{id}, 'last checked',
         $last_check_time, 'now', $now, 'diff', $now - ($last_check_time || 0),
         'hours until next check',
         sprintf '%.2f',
            ($check_time_limit - ($now - ($last_check_time || 0))) / 3600);
      if ( !defined $last_check_time
           || ($now - $last_check_time) >= $check_time_limit ) {
         PTDEBUG && _d('Time to check', Dumper($instance));
         push @instances_to_check, $instance;
      }
   }

   return \@instances_to_check;
}

sub update_check_times {
   my (%args) = @_;

   my $instances = $args{instances};
   my $now       = $args{now}     || int(time);
   my $vc_file   = $args{vc_file} || version_check_file();
   PTDEBUG && _d('Updating last check time:', $now);

   my %all_instances = map {
      $_->{id} => { name => $_->{name}, ts => $now }
   } @$instances;

   if ( -f $vc_file ) {
      open my $fh, '<', $vc_file or die "Cannot read $vc_file: $OS_ERROR";
      my $contents = do { local $/ = undef; <$fh> };
      close $fh;

      foreach my $line ( split("\n", ($contents || '')) ) {
         my ($id, $ts) = split(',', $line);
         if ( !exists $all_instances{$id} ) {
            $all_instances{$id} = { ts => $ts };  # original ts, not updated
         }
      }
   }

   open my $fh, '>', $vc_file or die "Cannot write to $vc_file: $OS_ERROR";
   foreach my $id ( sort keys %all_instances ) {
      PTDEBUG && _d('Updated:', $id, Dumper($all_instances{$id}));
      print { $fh } $id . ',' . $all_instances{$id}->{ts} . "\n";
   }
   close $fh;

   return;
}

sub get_instance_id {
   my ($instance) = @_;

   my $dbh = $instance->{dbh};
   my $dsn = $instance->{dsn};

   my $sql = q{SELECT CONCAT(@@hostname, @@port)};
   PTDEBUG && _d($sql);
   my ($name) = eval { $dbh->selectrow_array($sql) };
   if ( $EVAL_ERROR ) {
      PTDEBUG && _d($EVAL_ERROR);
      $sql = q{SELECT @@hostname};
      PTDEBUG && _d($sql);
      ($name) = eval { $dbh->selectrow_array($sql) };
      if ( $EVAL_ERROR ) {
         PTDEBUG && _d($EVAL_ERROR);
         $name = ($dsn->{h} || 'localhost') . ($dsn->{P} || 3306);
      }
      else {
         $sql = q{SHOW VARIABLES LIKE 'port'};
         PTDEBUG && _d($sql);
         my (undef, $port) = eval { $dbh->selectrow_array($sql) };
         PTDEBUG && _d('port:', $port);
         $name .= $port || '';
      }
   }
   my $id = md5_hex($name);

   PTDEBUG && _d('MySQL instance:', $id, $name, Dumper($dsn));

   return $name, $id;
}


sub pingback {
   my (%args) = @_;
   my @required_args = qw(url instances);
   foreach my $arg ( @required_args ) {
      die "I need a $arg arugment" unless $args{$arg};
   }
   my $url       = $args{url};
   my $instances = $args{instances};

   my $ua = $args{ua} || HTTPMicro->new( timeout => 3 );

   my $response = $ua->request('GET', $url);
   PTDEBUG && _d('Server response:', Dumper($response));
   die "No response from GET $url"
      if !$response;
   die("GET on $url returned HTTP status $response->{status}; expected 200\n",
       ($response->{content} || '')) if $response->{status} != 200;
   die("GET on $url did not return any programs to check")
      if !$response->{content};

   my $items = parse_server_response(
      response => $response->{content}
   );
   die "Failed to parse server requested programs: $response->{content}"
      if !scalar keys %$items;
      
   my $versions = get_versions(
      items     => $items,
      instances => $instances,
   );
   die "Failed to get any program versions; should have at least gotten Perl"
      if !scalar keys %$versions;

   my $client_content = encode_client_response(
      items      => $items,
      versions   => $versions,
      general_id => md5_hex( hostname() ),
   );

   my $client_response = {
      headers => { "X-Percona-Toolkit-Tool" => File::Basename::basename($0) },
      content => $client_content,
   };
   PTDEBUG && _d('Client response:', Dumper($client_response));

   $response = $ua->request('POST', $url, $client_response);
   PTDEBUG && _d('Server suggestions:', Dumper($response));
   die "No response from POST $url $client_response"
      if !$response;
   die "POST $url returned HTTP status $response->{status}; expected 200"
      if $response->{status} != 200;

   return unless $response->{content};

   $items = parse_server_response(
      response   => $response->{content},
      split_vars => 0,
   );
   die "Failed to parse server suggestions: $response->{content}"
      if !scalar keys %$items;
   my @suggestions = map { $_->{vars} }
                     sort { $a->{item} cmp $b->{item} }
                     values %$items;

   return \@suggestions;
}

sub encode_client_response {
   my (%args) = @_;
   my @required_args = qw(items versions general_id);
   foreach my $arg ( @required_args ) {
      die "I need a $arg arugment" unless $args{$arg};
   }
   my ($items, $versions, $general_id) = @args{@required_args};

   my @lines;
   foreach my $item ( sort keys %$items ) {
      next unless exists $versions->{$item};
      if ( ref($versions->{$item}) eq 'HASH' ) {
         my $mysql_versions = $versions->{$item};
         for my $id ( sort keys %$mysql_versions ) {
            push @lines, join(';', $id, $item, $mysql_versions->{$id});
         }
      }
      else {
         push @lines, join(';', $general_id, $item, $versions->{$item});
      }
   }

   my $client_response = join("\n", @lines) . "\n";
   return $client_response;
}

sub parse_server_response {
   my (%args) = @_;
   my @required_args = qw(response);
   foreach my $arg ( @required_args ) {
      die "I need a $arg arugment" unless $args{$arg};
   }
   my ($response) = @args{@required_args};

   my %items = map {
      my ($item, $type, $vars) = split(";", $_);
      if ( !defined $args{split_vars} || $args{split_vars} ) {
         $vars = [ split(",", ($vars || '')) ];
      }
      $item => {
         item => $item,
         type => $type,
         vars => $vars,
      };
   } split("\n", $response);

   PTDEBUG && _d('Items:', Dumper(\%items));

   return \%items;
}

my %sub_for_type = (
   os_version          => \&get_os_version,
   perl_version        => \&get_perl_version,
   perl_module_version => \&get_perl_module_version,
   mysql_variable      => \&get_mysql_variable,
);

sub valid_item {
   my ($item) = @_;
   return unless $item;
   if ( !exists $sub_for_type{ $item->{type} } ) {
      PTDEBUG && _d('Invalid type:', $item->{type});
      return 0;
   }
   return 1;
}

sub get_versions {
   my (%args) = @_;
   my @required_args = qw(items);
   foreach my $arg ( @required_args ) {
      die "I need a $arg arugment" unless $args{$arg};
   }
   my ($items) = @args{@required_args};

   my %versions;
   foreach my $item ( values %$items ) {
      next unless valid_item($item);
      eval {
         my $version = $sub_for_type{ $item->{type} }->(
            item      => $item,
            instances => $args{instances},
         );
         if ( $version ) {
            chomp $version unless ref($version);
            $versions{$item->{item}} = $version;
         }
      };
      if ( $EVAL_ERROR ) {
         PTDEBUG && _d('Error getting version for', Dumper($item), $EVAL_ERROR);
      }
   }

   return \%versions;
}


sub get_os_version {
   if ( $OSNAME eq 'MSWin32' ) {
      require Win32;
      return Win32::GetOSDisplayName();
   }

  chomp(my $platform = `uname -s`);
  PTDEBUG && _d('platform:', $platform);
  return $OSNAME unless $platform;

   chomp(my $lsb_release
            = `which lsb_release 2>/dev/null | awk '{print \$1}'` || '');
   PTDEBUG && _d('lsb_release:', $lsb_release);

   my $release = "";

   if ( $platform eq 'Linux' ) {
      if ( -f "/etc/fedora-release" ) {
         $release = `cat /etc/fedora-release`;
      }
      elsif ( -f "/etc/redhat-release" ) {
         $release = `cat /etc/redhat-release`;
      }
      elsif ( -f "/etc/system-release" ) {
         $release = `cat /etc/system-release`;
      }
      elsif ( $lsb_release ) {
         $release = `$lsb_release -ds`;
      }
      elsif ( -f "/etc/lsb-release" ) {
         $release = `grep DISTRIB_DESCRIPTION /etc/lsb-release`;
         $release =~ s/^\w+="([^"]+)".+/$1/;
      }
      elsif ( -f "/etc/debian_version" ) {
         chomp(my $rel = `cat /etc/debian_version`);
         $release = "Debian $rel";
         if ( -f "/etc/apt/sources.list" ) {
             chomp(my $code_name = `awk '/^deb/ {print \$3}' /etc/apt/sources.list | awk -F/ '{print \$1}'| awk 'BEGIN {FS="|"} {print \$1}' | sort | uniq -c | sort -rn | head -n1 | awk '{print \$2}'`);
             $release .= " ($code_name)" if $code_name;
         }
      }
      elsif ( -f "/etc/os-release" ) { # openSUSE
         chomp($release = `grep PRETTY_NAME /etc/os-release`);
         $release =~ s/^PRETTY_NAME="(.+)"$/$1/;
      }
      elsif ( `ls /etc/*release 2>/dev/null` ) {
         if ( `grep DISTRIB_DESCRIPTION /etc/*release 2>/dev/null` ) {
            $release = `grep DISTRIB_DESCRIPTION /etc/*release | head -n1`;
         }
         else {
            $release = `cat /etc/*release | head -n1`;
         }
      }
   }
   elsif ( $platform =~ m/(?:BSD|^Darwin)$/ ) {
      my $rel = `uname -r`;
      $release = "$platform $rel";
   }
   elsif ( $platform eq "SunOS" ) {
      my $rel = `head -n1 /etc/release` || `uname -r`;
      $release = "$platform $rel";
   }

   if ( !$release ) {
      PTDEBUG && _d('Failed to get the release, using platform');
      $release = $platform;
   }
   chomp($release);

   $release =~ s/^"|"$//g;

   PTDEBUG && _d('OS version =', $release);
   return $release;
}

sub get_perl_version {
   my (%args) = @_;
   my $item = $args{item};
   return unless $item;

   my $version = sprintf '%vd', $PERL_VERSION;
   PTDEBUG && _d('Perl version', $version);
   return $version;
}

sub get_perl_module_version {
   my (%args) = @_;
   my $item = $args{item};
   return unless $item;

   my $var     = '$' . $item->{item} . '::VERSION';
   my $version = eval "use $item->{item}; $var;";
   PTDEBUG && _d('Perl version for', $var, '=', $version);
   return $version;
}

sub get_mysql_variable {
   return get_from_mysql(
      show => 'VARIABLES',
      @_,
   );
}

sub get_from_mysql {
   my (%args) = @_;
   my $show      = $args{show};
   my $item      = $args{item};
   my $instances = $args{instances};
   return unless $show && $item;

   if ( !$instances || !@$instances ) {
      PTDEBUG && _d('Cannot check', $item,
         'because there are no MySQL instances');
      return;
   }

   if ($item->{item} eq 'MySQL' && $item->{type} eq 'mysql_variable') {
       $item->{vars} = ['version_comment', 'version'];
   }

   my @versions;
   my %version_for;
   foreach my $instance ( @$instances ) {
      next unless $instance->{id};  # special system instance has id=0
      my $dbh = $instance->{dbh};
      local $dbh->{FetchHashKeyName} = 'NAME_lc';
      my $sql = qq/SHOW $show/;
      PTDEBUG && _d($sql);
      my $rows = $dbh->selectall_hashref($sql, 'variable_name');

      my @versions;
      foreach my $var ( @{$item->{vars}} ) {
         $var = lc($var);
         my $version = $rows->{$var}->{value};
         PTDEBUG && _d('MySQL version for', $item->{item}, '=', $version,
            'on', $instance->{name});
         push @versions, $version;
      }
      $version_for{ $instance->{id} } = join(' ', @versions);
   }

   return \%version_for;
}

sub _d {
   my ($package, undef, $line) = caller 0;
   @_ = map { (my $temp = $_) =~ s/\n/\n# /g; $temp; }
        map { defined $_ ? $_ : 'undef' }
        @_;
   print STDERR "# $package:$line $PID ", join(' ', @_), "\n";
}

1;
}
# ###########################################################################
# End VersionCheck package
# ###########################################################################

#
# parse_connection_options() subroutine parses connection-related command line
# options
#
sub parse_connection_options {
    my $con = shift;

    $con->{dsn} = 'dbi:mysql:';

    # this option has to be first
    if ($ENV{option_defaults_file}) {
        $con->{dsn} .= ";mysql_read_default_file=$ENV{option_defaults_file}";
    }

    if ($ENV{option_defaults_extra_file}) {
        $con->{dsn} .= ";mysql_read_default_file=$ENV{option_defaults_extra_file}";
    }

    $con->{dsn} .= ";mysql_read_default_group=xtrabackup";

    if ($ENV{option_mysql_password}) {
        $con->{dsn_password} = "$ENV{option_mysql_password}";
    }
    if ($ENV{option_mysql_user}) {
        $con->{dsn_user} = "$ENV{option_mysql_user}";
    }
    if ($ENV{option_mysql_host}) {
        $con->{dsn} .= ";host=$ENV{option_mysql_host}";
    }
    if ($ENV{option_mysql_port}) {
        $con->{dsn} .= ";port=$ENV{option_mysql_port}";
    }
    if ($ENV{option_mysql_socket}) {
        $con->{dsn} .= ";mysql_socket=$ENV{option_mysql_socket}";
    }
}

#
# mysql_connect subroutine connects to MySQL server
#
sub mysql_connect {
    my %con;
    my %args = (
                  # Defaults
                  abort_on_error => 1,
                  @_
               );

    $con{abort_on_error} = $args{abort_on_error};

    parse_connection_options(\%con);

    $now = current_time();
    print STDERR "$now  $prefix Connecting to MySQL server with DSN '$con{dsn}'" .
        (defined($con{dsn_user}) ? " as '$con{dsn_user}' " : "") .
        " (using password: ";
    if (defined($con{dsn_password})) {
        print STDERR "YES).\n";
    } else {
        print STDERR "NO).\n";
    }

    eval {
        $con{dbh}=DBI->connect($con{dsn}, $con{dsn_user},
                                 $con{dsn_password}, { RaiseError => 1 });
    };

    if ($EVAL_ERROR) {
        $con{connect_error}=$EVAL_ERROR;
    } else {
        $now = current_time();
        print STDERR "$now  $prefix Connected to MySQL server\n";
    }

    if ($args{abort_on_error}) {
        if (!$dbd_mysql_installed) {
            die "Failed to connect to MySQL server as " .
                "DBD::mysql module is not installed";
        } else {
            if (!$con{dbh}) {
                die "Failed to connect to MySQL server: " .
                    $con{connect_error};
            }
        }
    }

    if ($con{dbh}) {
        $con{dbh}->do("SET SESSION wait_timeout=2147483");
    }

    return %con;
}

#
# return current local time as string in form "070816 12:23:15"
#
sub current_time {
    return strftime("%y%m%d %H:%M:%S", localtime());
}


%mysql = mysql_connect(abort_on_error => 1);

$now = current_time();
print STDERR
    "$now  $prefix Executing a version check against the server...\n";

# Redirect STDOUT to STDERR, as VersionCheck prints alerts to STDOUT
select STDERR;

VersionCheck::version_check(
                            force => 1,
                            instances => [ {
                                            dbh => $mysql{dbh},
                                            dsn => $mysql{dsn}
                                           }
                                         ]
                            );
# Restore STDOUT as the default filehandle
select STDOUT;

$now = current_time();
print STDERR "$now  $prefix Done.\n";
