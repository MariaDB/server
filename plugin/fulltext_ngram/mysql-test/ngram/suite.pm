package My::Suite::Ngram;

@ISA = qw(My::Suite);

return "No ngram plugin" unless $ENV{FULLTEXT_NGRAM_SO} or
        $::mysqld_variables{'ngram'} eq "ON";

sub is_default { 1 }

bless { };
