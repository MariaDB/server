# Sets $disconnect_watcher to 1 if the disconnect_watcher is ON, and to 0 if it
# is OFF or does not exist, which is the case on the systems without one.
let $disconnect_watcher=`SELECT COUNT(*) FROM information_schema.global_variables
  WHERE variable_name = 'DISCONNECT_WATCHER' AND variable_value = 'ON'`;
