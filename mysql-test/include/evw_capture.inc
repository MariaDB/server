# Capture one execution method of $EVW_QRY into the file $EVW_OUT.
#
# Used by main/execute_various_ways.inc to cross-check that a query run
# several different ways returns the same rows.
#
# The statements are piped through a fresh mysql client session (so the
# real PREPARE/EXECUTE and CALL paths are exercised, unlike a backtick
# capture which only returns the first row, or a CREATE/INSERT ... SELECT
# sink which would change sql_command away from SQLCOM_SELECT).
#
# A marker row is emitted immediately before the row-producing statement;
# only the lines after the LAST marker are kept.  For the prepared
# statement and stored procedure methods this isolates the *2nd* execution
# (the one MDEV-35673 is about).  Kept rows are sorted, so row order does
# not need to be deterministic in $EVW_QRY.
#
# In (environment variables, set with `--let NAME= value`):
#   MYSQL        - the mysql client command (exported by mtr)
#   EVW_DB       - database to run in
#   EVW_QRY      - the query under test.  It is interpolated into SQL built
#                  here (e.g. prepare s from '$q'), so it must contain no
#                  interior ';' and no single quote (').
#   EVW_METHOD   - 0..5, selects the execution method
#   EVW_MARKER   - marker string separating discarded from kept output
#   EVW_OUT      - output file for the normalised rows

--perl
  my ($mysql,$db,$q,$m,$mark,$out) =
     @ENV{qw(MYSQL EVW_DB EVW_QRY EVW_METHOD EVW_MARKER EVW_OUT)};

  my $sel;
  if    ($m==0){ $sel="select '$mark'; $q"; }
  elsif ($m==1){ $sel="prepare s from '$q'; execute s; select '$mark'; execute s; deallocate prepare s"; }
  elsif ($m==2){ $sel="select '$mark'; select * from ($q) dt"; }
  elsif ($m==3){ $sel="create view evw_v as $q; select '$mark'; select * from evw_v; drop view evw_v"; }
  elsif ($m==4){ $sel="select '$mark'; with cte as ($q) select * from cte"; }
  elsif ($m==5){ $sel="create procedure evw_p() $q; call evw_p(); select '$mark'; call evw_p(); drop procedure evw_p"; }
  else         { die "evw_capture.inc: bad EVW_METHOD '$m'"; }

  my $raw="$out.raw";
  # stderr is folded into the capture (2>&1); a method that errors *before*
  # the marker would yield empty output, so this relies on the visible
  # section of execute_various_ways.inc running first and aborting the test
  # on any unexpected error.
  open(P,"| $mysql --batch --skip-column-names $db > $raw 2>&1")
      or die "evw_capture.inc: pipe to mysql: $!";
  print P "$sel;\n" or die "evw_capture.inc: write to mysql: $!";
  close(P) or die "evw_capture.inc: mysql client failed (exit $?) for method $m";

  open(R,"<",$raw) or die "evw_capture.inc: read $raw: $!";
  my @l=<R>; close(R); unlink $raw;
  my $start=0;
  for my $i (0..$#l){ $start=$i+1 if $l[$i]=~/\Q$mark\E/; }
  my @rows= sort @l[$start..$#l];
  open(W,">",$out) or die "evw_capture.inc: write $out: $!";
  print W @rows; close(W);
EOF
