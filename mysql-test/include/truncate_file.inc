# truncate a giving file, all contents of the file are be cleared

if (!$TRUNCATE_FILE)
{
  die TRUNCATE_FILE is not set;
}

perl;
open FILE, '>', $ENV{TRUNCATE_FILE} or die "open(>$ENV{TRUNCATE_FILE}): $!";
close FILE;
EOF
