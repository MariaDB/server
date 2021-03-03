package My::Suite::Sysschema;

@ISA = qw(My::Suite);

use strict;

return "Need perfschema engine" unless exists($::mysqld_variables{'performance-schema'}) ;
bless { };
