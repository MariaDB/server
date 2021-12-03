--echo Loading wsrep provider ...

--disable_query_log
#
# count occurences of successful node starts in error log
#
perl;
  use strict;
   my $test_log=$ENV{'LOG_FILE'} or die "LOG_FILE not set";
   my $test_log_copy=$test_log . '.copy';
   if (-e $test_log_copy) {
      unlink $test_log_copy;
   }

EOF
--copy_file $LOG_FILE $LOG_FILE.copy

#
#  now join to the cluster
#
--eval SET GLOBAL wsrep_cluster_address = '$wsrep_cluster_address_orig';

--enable_query_log

#
# Cluster address change above, will launch SST/IST
# if mysqldump ST has been configured, mysqld will close all
# client connections, and it will be hard for us to poll for
# cluster status during SST process, therefore wait_until_connected_again.inc
# and wait_until_ready.inc may fail in this phase
# To workaround this, we do first lazy polling here just to see when
# client connections will be possible, and after that check for node readyness
#
--disable_result_log
--disable_query_log

--error 0,1
perl;
  use strict;
  my $logfile = $ENV{'LOG_FILE'} or die ("no error log file set");

  my $counter = 1000;
  #my $found = false
  
  while ($counter > 0) {
 
    open(FILE, "$logfile") or die("Unable to open $logfile : $!\n");
    my $new_sync_count = () = grep(/Synchronized with group/g,<FILE>);
    close(FILE);

    open(FILEN, "$logfile.copy") or die("Unable to open $logfile.copy : $!\n");
    my $old_sync_count = () = grep(/Synchronized with group/g,<FILEN>);
    close(FILEN);

    if ($new_sync_count > $old_sync_count ) {
       exit(0);
    }
    $counter--;
    sleep(5);
  }
  exit(1);
EOF
if ($errno)
{
--echo "SST failed $errno"
}

--remove_file $LOG_FILE.copy

--enable_query_log
--enable_result_log
#--eval SET GLOBAL log_error = $log_error_;

--source include/galera_wait_ready.inc
