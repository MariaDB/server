package My::Suite::Plugins;

use My::Platform;

@ISA = qw(My::Suite);

sub cassandra_running() { 
  return 0 if IS_WINDOWS; 
  system 'echo show version | cqlsh -3 2>/dev/null >/dev/null'; 
  return $? == 0; 
} 

sub skip_combinations {
  my %skip;
  $skip{'t/pam.test'} = 'No pam setup for mtr'
             unless -e '/etc/pam.d/mariadb_mtr';
  $skip{'t/cassandra.test'} = 'Cassandra is not running'
             unless cassandra_running();
  $skip{'t/cassandra_qcache.test'} = $skip{'t/cassandra.test'};
  %skip;
}

bless { };

