#!/usr/bin/perl

my $table_name= $ARGV[0];
my $id1= 1;
my $id2= 1;
my $id3= 1;
my $id4= 1;
my $id5= 1;
my $value= 1000;
my $value2= 'aaabbbccc';
my $max_rows = 1 * 10000;

for(my $row_id= 1; $row_id <= $max_rows; $row_id++) {
  my $value_clause = "($id1, $id2, $id3, $id4, $id5, $value, \"$value2\")";

  if ($row_id % 100 == 1) {
    print "INSERT INTO $table_name VALUES";
  }

  if ($row_id % 100 == 0) {
    print "$value_clause;\n";
  }else {
    print "$value_clause,";
  }

  $id4++;
  $id5++;
  $id3++ if($row_id % 5 == 0);
  $id2++ if($row_id % 5 == 0);
  $id1++ if($row_id % 10 == 0);
}

