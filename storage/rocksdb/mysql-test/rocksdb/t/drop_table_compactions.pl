sub print_array {
  $str = shift;
  @arr = @_;
  $prev= 0;
  foreach (@arr) {
    if ($prev) {
      $dummy_idx = $_ - $prev;
    }else {
      $dummy_idx = 0;
    }
    $prev= $_;
    print "$str $dummy_idx\n";
  }
}

while (<>) {
  if (/Compacting away elements from dropped index \(\d+,(\d+)\): (\d+)/) {
    $a{$1} += $2;
  }
  if (/Begin filtering dropped index \(\d+,(\d+)\)/) {
    push @b, $1;
  }
  if (/Finished filtering dropped index \(\d+,(\d+)\)/) {
    push @c, $1;
  }
}
$prev= 0;
foreach (sort {$a <=> $b} keys %a){
  if ($prev) {
    $dummy_idx= $_ - $prev;
  }else {
    $dummy_idx= 0;
  }
  $prev= $_;
}
print_array("Begin filtering dropped index+", sort {$a <=> $b} @b);
print_array("Finished filtering dropped index+", sort {$a <=> $b} @c);
