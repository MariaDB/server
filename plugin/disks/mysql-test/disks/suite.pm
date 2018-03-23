package My::Suite::Disks;

@ISA = qw(My::Suite);

return "No Disks plugin" unless $ENV{DISKS_SO};

sub is_default { 1 }

bless { };

