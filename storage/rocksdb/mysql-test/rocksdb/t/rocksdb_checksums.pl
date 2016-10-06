$file=$ARGV[0];
$total=$ARGV[1];
$pct=$ARGV[2];

open($fh, "<", $file) or die $!;
while(readline($fh)) {
  if (/(\d+) index entries checked \((\d+) had checksums/) {
    if ($1 == $total && $2 >= $total*($pct-2)/100 && $2 <= $total*($pct+2)/100) {
      printf("%d index entries had around %d checksums\n", $total, $total*$pct/100);
    }
  }elsif (/(\d+) table records had checksums/) {
    if ($1 >= $total*($pct-2)/100 && $1 <= $total*($pct+2)/100) {
      printf("Around %d table records had checksums\n", $total*$pct/100);
    }
  }
}
