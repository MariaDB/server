# run a check script to verify sst files reduced enough during each optimize table
perl;
use autodie qw(open);

$size += -s $_ for (<$ENV{datadir}/.rocksdb/*.sst>);
$file= "$ENV{MYSQL_TMP_DIR}/sst_size.dat";

if (-f $file) {
  open(F, '<', $file);
  $old = <F>;
  close F;
  if ($old - $size < 1e6) {
    print "sst file reduction was not enough $old -> $size (minimum 1000kb)\n";
  } else {
    print "sst file reduction ok\n";
  }
}
open(F, '>', $file);
print F $size;
close F;
EOF
