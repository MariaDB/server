
# Based off support_long_file_names.inc

--error 0,3
--perl

my $ok= 0;
foreach my $large_page (glob '/sys/kernel/mm/hugepages/*/free_hugepages')
{
  open FILE, '<', $large_page || continue;
  $free= <FILE>;
  close FILE;
  if ($free > 0)
  {
    $ok= 1;
  }
}
open (F, '>' . $ENV{'MYSQL_TMP_DIR'} . '/large_pages.inc');
print F "let \$large_pages= $ok;\n";
close F;
EOF

--source $MYSQL_TMP_DIR/large_pages.inc
--remove_file $MYSQL_TMP_DIR/large_pages.inc


if (!$large_pages)
{
  --skip No large pages available
}
