#! /bin/sh

# This is a script for testing regular expressions with Perl to check that
# it handles them the same way as PCRE2. For testing with different versions of
# Perl, if the first argument is -perl, the second is taken as the Perl command
# to use, and both are then removed. If the next argument is "-w", Perl is
# called with "-w", which turns on its warning mode.
#
# The Perl code has to have "use utf8" and "require Encode" at the start when
# running UTF-8 tests, but *not* for non-utf8 tests. The "require" would
# actually be OK for non-utf8-tests, but is not always installed, so this way
# the script will always run for these tests.
#
# The desired effect is achieved by making this a shell script that passes the
# a script to Perl through a pipe. See comments below about the data for the
# Perl script. If the next argument of this script is "-utf8", a suitable
# prefix for the Perl script is set up.
#
# A similar process is used to indicate the desire to set a specific locale
# tables per pattern in a similar way to pcre2test through a locale modifier,
# by using the -locale argument. This can be optionally combined with the
# previous arguments; for example, to process an UTF-8 test file in Turkish,
# add the locale=tr_TR.utf8 modifier to the pattern and -locale to perltest,
# or invoke something like (the specific names of the locale might vary):
#
#   ./perltest.sh -utf8 -locale=tr_TR.utf8 some-file
#
# If the -locale argument has no setting, a suitable default locale is used
# when possible and reported at startup, it can be always overriden using the
# locale modifier for each pattern.
#
# The remaining arguments of this script, if any, are passed to Perl. They are
# an input file and an output file. If there is one argument, the output is
# written to STDOUT. If Perl receives no arguments, it opens /dev/tty as input,
# and writes output to STDOUT. (I haven't found a way of getting it to use
# STDIN, because of the contorted piping input.)


# Handle the shell script arguments.

perl=perl
perlarg=""
prefix=""
spc=""

if [ $# -gt 0 -a "$1" = "-perl" ] ; then
  if [ $# -lt 2 ] ; then
    echo "perltest.sh: Missing perl command after -perl"
    exit 1
  fi
  shift
  perl=$1
  shift
fi

if [ $# -gt 0 -a "$1" = "-w" ] ; then
  perlarg="-w"
  spc=" "
  shift
fi

if [ $# -gt 0 -a "$1" = "-utf8" ] ; then
  default_locale="C.utf8"
  prefix="\
  use utf8;\
  require Encode;"
  perlarg="$perlarg$spc-CSD"
  shift
fi

if [ $# -gt 0 ] ; then
  case "$1" in
  -locale=*)
    default_locale=${1#-locale=}
    ;;
  -locale)
    default_locale=${default_locale:-C}
    ;;
  *)
    skip=1
  esac
  if [ -z "$skip" ] ; then
    prefix="\
    use POSIX qw(locale_h);\
    use locale qw(:ctype);\
    \
    \$default_locale = setlocale(LC_CTYPE, \"$default_locale\");\
    if (!defined(\$default_locale))\
      { die \"perltest: Failed to set locale \\\"$default_locale\\\"\\\n\"; }\
    print \"Locale: \$default_locale\\\n\";\
    $prefix"
    shift
  fi
fi


# The Perl script that follows has a similar specification to pcre2test, and so
# can be given identical input, except that input patterns can be followed only
# by Perl's lower case modifiers and certain other pcre2test modifiers that are
# either handled or ignored:
#
#   aftertext          interpreted as "print $' afterwards"
#   afteralltext       ignored
#   dupnames           ignored (Perl always allows)
#   hex                preprocess pattern with embedded octets
#   jitstack           ignored
#   locale             use a specific locale tables
#   mark               show mark information
#   no_auto_possess    ignored
#   no_start_optimize  insert (??{""}) at pattern start (disables optimizing)
#  -no_start_optimize  ignored
#   subject_literal    does not process subjects for escapes
#   ucp                sets Perl's /u modifier
#   utf                invoke UTF-8 functionality
#
# Comment lines are ignored. The #pattern command can be used to set modifiers
# that will be added to each subsequent pattern, after any modifiers it may
# already have. NOTE: this is different to pcre2test where #pattern sets
# defaults which can be overridden on individual patterns. The #subject command
# may be used to set or unset a default "mark" modifier for data lines. This is
# the only use of #subject that is supported. The #perltest, #forbid_utf,
# #newline_default, and #if...#endif commands, which are needed in the relevant
# pcre2test files, are ignored. Any other #-command is ignored, with a warning
# message.
#
# The pattern lines should use only / as the delimiter. The other characters
# that pcre2test supports cause problems with this script.
#
# The data lines must not have any pcre2test modifiers. Unless
# "subject_literal" is on the pattern, data lines are processed as
# Perl double-quoted strings, so if they contain " $ or @ characters, these
# have to be escaped. For this reason, all such characters in the
# Perl-compatible testinput1 and testinput4 files are escaped so that they can
# be used for perltest as well as for pcre2test. The output from this script
# should be same as from pcre2test, apart from the initial identifying banner.
#
# The other testinput files are not suitable for feeding to perltest.sh,
# because they make use of the special modifiers that pcre2test uses for
# testing features of PCRE2. Some of these files also contain malformed regular
# expressions, in order to check that PCRE2 diagnoses them correctly.

(echo "$prefix" ; cat <<'PERLEND'

# Avoid warnings for some of the experimental features that are being used.

no warnings "experimental::alpha_assertions";
no warnings "experimental::script_run";
no warnings "experimental::vlb";

# Function for turning a string into a string of printing chars.

sub pchars {
my($t) = "";
if ($utf8)
  {
  @p = unpack('U*', $_[0]);
  foreach $c (@p)
    {
    if ($c >= 32 && $c < 127) { $t .= chr $c; }
      else { $t .= sprintf("\\x{%02x}", $c);
      }
    }
  }
else
  {
  foreach $c (split(//, $_[0]))
    {
    if ($c =~ /^[[:print:]]$/) { $t .= $c; }
      else { $t .= sprintf("\\x%02x", ord $c); }
    }
  }
$t;
}


# Read lines from a named file or stdin and write to a named file or stdout;
# lines consist of a regular expression, in delimiters and optionally followed
# by options, followed by a set of test data, terminated by an empty line.

# Sort out the input and output files

if (@ARGV > 0)
  {
  open(INFILE, "<$ARGV[0]") || die "Failed to open $ARGV[0]\n";
  $infile = "INFILE";
  $interact = 0;
  }
else
  {
  open(INFILE, "</dev/tty") || die "Failed to open /dev/tty\n";
  $infile = "INFILE";
  $interact = 1;
  }

if (@ARGV > 1)
  {
  open(OUTFILE, ">$ARGV[1]") || die "Failed to open $ARGV[1]\n";
  $outfile = "OUTFILE";
  }
else { $outfile = "STDOUT"; }

printf($outfile "Perl $^V\n");

$extra_modifiers = "";
$default_show_mark = 0;

# Main loop

NEXT_RE:
for (;;)
  {
  if (defined $locale && defined $default_locale)
    {
    setlocale(LC_CTYPE, $default_locale);
    undef $locale;
    }

  printf "  re> " if $interact;
  last if ! ($_ = <$infile>);
  printf $outfile "$_" if ! $interact;
  next if ($_ =~ /^\s*$/ || $_ =~ /^#[\s!]/);

  # A few of pcre2test's #-commands are supported, or just ignored. Any others
  # cause an error.

  if ($_ =~ /^#pattern(.*)/)
    {
    $extra_modifiers = $1;
    chomp($extra_modifiers);
    $extra_modifiers =~ s/\s+$//;
    next;
    }
  elsif ($_ =~ /^#subject(.*)/)
    {
    $mod = $1;
    chomp($mod);
    $mod =~ s/\s+$//;
    if ($mod =~ s/(-?)mark,?//)
      {
      $minus = $1;
      $default_show_mark = ($minus =~ /^$/);
      }
    if ($mod !~ /^\s*$/)
      {
      printf $outfile "** Warning: \"$mod\" in #subject ignored\n";
      }
    next;
    }
  elsif ($_ =~ /^#/)
    {
    if ($_ !~ /^#newline_default|^#perltest|^#forbid_utf|^#if|^#endif/)
      {
      printf $outfile "** Warning: #-command ignored: %s", $_;
      }
    next;
    }

  $pattern = $_;

  while ($pattern !~ /^\s*(.).*\1/s)
    {
    printf "    > " if $interact;
    last if ! ($_ = <$infile>);
    printf $outfile "$_" if ! $interact;
    $pattern .= $_;
    }

  chomp($pattern);
  $pattern =~ s/\s+$//;

  # Split the pattern from the modifiers and adjust them as necessary.

  $pattern =~ /^\s*(.)(.*)\1(.*)$/s;
  $del = $1;
  $pat = $2;
  $mod = "$3,$extra_modifiers";
  $mod =~ s/^,\s*//;

  # The private "aftertext" modifier means "print $' afterwards".

  $showrest = ($mod =~ s/aftertext,?//);

  # The "subject_literal" modifier disables escapes in subjects.

  $subject_literal = ($mod =~ s/subject_literal,?//);

  # "allaftertext" is used by pcre2test to print remainders after captures

  $mod =~ s/allaftertext,?//;

  # Remove "dupnames".

  $mod =~ s/dupnames,?//;

  # Remove "jitstack".

  $mod =~ s/jitstack=\d+,?//;

  # The "locale" modifier indicates which locale to use
  if ($mod =~ /locale=([^,]+),?/)
    {
    die "perltest: missing -locale cmdline flag" unless defined &setlocale;
    $locale = setlocale(LC_CTYPE, $1);
    if (!defined $locale)
      {
      print "** Failed to set locale '$1'\n";
      next NEXT_RE;
      }
    }
  $mod =~ s/locale=[^,]*,?//;                # Remove it; "locale=" Ignored

  # The "mark" modifier requests checking of MARK data */

  $show_mark = $default_show_mark | ($mod =~ s/mark,?//);

  # "ucp" asks pcre2test to set PCRE2_UCP; change this to /u for Perl

  $mod =~ s/ucp,?/u/;

  # Detect utf

  $utf8 = $mod =~ s/utf,?//;

  # Remove "no_auto_possess".

  $mod =~ s/no_auto_possess,?//;

  # The "hex" modifier instructs us to preprocess a pattern with embedded
  # octets formatted as two digit hexadecimals

  if ($mod =~ s/hex,?//)
    {
    my $t = "";

    # find either 2 digit hex octets, optionally surrounded by spaces, to
    # add as code points or quoted strings that will be copied verbatim

    while ($pat =~ /\s*(?:(\p{ahex}{2})|(['"])([^\2]+?)\2)\s*/g)
      {
      if (defined $1)
        {
        no utf8;
        $t .= chr(hex($1));
        use if $utf8, "utf8";
        }
      else
        {
        $t .= $3;
        }
      }
    no utf8;
    utf8::decode($t) if $utf8;
    use if $utf8, "utf8";
    $pat = $t;
    }

  # Use no_start_optimize (disable PCRE2 start-up optimization) to disable Perl
  # optimization by inserting (??{""}) at the start of the pattern. We may
  # also encounter -no_start_optimize from a #pattern setting.

  $mod =~ s/-no_start_optimize,?//;

  if ($mod =~ s/no_start_optimize,?//) { $pat = '(??{""})' . $pat; }

  # Add back retained modifiers and check that the pattern is valid.

  $mod =~ s/,//g;
  $pattern = "$del$pat$del$mod";

  eval "\$_ =~ ${pattern}";
  if ($@)
    {
    printf $outfile "Error: $@";
    if (! $interact)
      {
      for (;;)
        {
        last if ! ($_ = <$infile>);
        last if $_ =~ /^\s*$/;
        }
      }
    next NEXT_RE;
    }

  # If the /g modifier is present, we want to put a loop round the matching;
  # otherwise just a single "if".

  $cmd = ($pattern =~ /g[a-z]*\s*$/)? "while" : "if";

  # If the pattern is actually the null string, Perl uses the most recently
  # executed (and successfully compiled) regex is used instead. This is a
  # nasty trap for the unwary! The PCRE2 test suite does contain null strings
  # in places - if they are allowed through here all sorts of weird and
  # unexpected effects happen. To avoid this, we replace such patterns with
  # a non-null pattern that has the same effect.

  $pattern = "/(?#)/$2" if ($pattern =~ /^(.)\1(.*)$/);

  # Read data lines and test them

  for (;;)
    {
    printf "data> " if $interact;
    last NEXT_RE if ! ($_ = <$infile>);
    chomp;
    printf $outfile "%s", "$_\n" if ! $interact;

    s/\s+$//;  # Remove trailing space
    s/^\s+//;  # Remove leading space

    last if ($_ eq "");
    next if $_ =~ /^\\=(?:\s|$)/;   # Comment line

    if ($subject_literal)
      {
      $x = $_;
      }
    else
      {
      s/(?<!\\)\\$//;     # Remove pcre2test specific trailing backslash
      $x = eval "\"$_\""; # To get escapes processed
      if ($interact && $@)
        {
        print STDERR "$@";
        redo;
        }
      }

    # Empty array for holding results, ensure $REGERROR and $REGMARK are
    # unset, then do the matching.

    @subs = ();

    $pushes = "push \@subs,\$&;" .
         "push \@subs,\$1;" .
         "push \@subs,\$2;" .
         "push \@subs,\$3;" .
         "push \@subs,\$4;" .
         "push \@subs,\$5;" .
         "push \@subs,\$6;" .
         "push \@subs,\$7;" .
         "push \@subs,\$8;" .
         "push \@subs,\$9;" .
         "push \@subs,\$10;" .
         "push \@subs,\$11;" .
         "push \@subs,\$12;" .
         "push \@subs,\$13;" .
         "push \@subs,\$14;" .
         "push \@subs,\$15;" .
         "push \@subs,\$16;" .
         "push \@subs,\$'; }";

    undef $REGERROR;
    undef $REGMARK;

    eval "${cmd} (\$x =~ ${pattern}) {" . $pushes;

    if ($@)
      {
      printf $outfile "Error: $@";
      next NEXT_RE;
      }
    elsif (scalar(@subs) == 0)
      {
      printf $outfile "No match";
      if ($show_mark && defined $REGERROR && $REGERROR != 1)
        { printf $outfile (", mark = %s", &pchars($REGERROR)); }
      printf $outfile "\n";
      }
    else
      {
      while (scalar(@subs) != 0)
        {
        printf $outfile (" 0: %s\n", &pchars($subs[0]));
        printf $outfile (" 0+ %s\n", &pchars($subs[17])) if $showrest;
        $last_printed = 0;
        for ($i = 1; $i <= 16; $i++)
          {
          if (defined $subs[$i])
            {
            while ($last_printed++ < $i-1)
              { printf $outfile ("%2d: <unset>\n", $last_printed); }
            printf $outfile ("%2d: %s\n", $i, &pchars($subs[$i]));
            $last_printed = $i;
            }
          }
        splice(@subs, 0, 18);
        }

      # It seems that $REGMARK is not marked as UTF-8 even when use utf8 is
      # set and the input pattern was a UTF-8 string. We can, however, force
      # it to be so marked.

      if ($show_mark && defined $REGMARK && $REGMARK != 1)
        {
        $xx = $REGMARK;
        $xx = Encode::decode_utf8($xx) if $utf8;
        printf $outfile ("MK: %s\n", &pchars($xx));
        }
      }
    }
  }

# By closing OUTFILE explicitly, we avoid a Perl warning in -w mode
# "main::OUTFILE" used only once".

close(OUTFILE) if $outfile eq "OUTFILE";

PERLEND
) | $perl $perlarg - $@

# End
