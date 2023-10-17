--disable_ps2_protocol
eval select "Outfile OK" into outfile "$MYSQLTEST_VARDIR/tmp/outfile.test";
--enable_ps2_protocol
