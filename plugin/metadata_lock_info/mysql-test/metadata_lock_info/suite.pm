package My::Suite::Metadata_lock_info;

@ISA = qw(My::Suite);

return "No Metadata_lock_info plugin" unless $ENV{METADATA_LOCK_INFO_SO} or
  $::mysqld_variables{'metadata-lock-info'} eq "ON";;

sub is_default { 1 }

bless { };

