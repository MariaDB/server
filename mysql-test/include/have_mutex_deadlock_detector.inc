if (`select count(*)=0 from information_schema.global_variables where variable_name = "debug_mutex_deadlock_detector"`) {
  skip needs safemutex deadlock detector;
}
