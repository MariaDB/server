package My::Suite::Ngram;

@ISA = qw(My::Suite);

return "No fulltext_ngram plugin" unless $ENV{FULLTEXT_NGRAM_SO};

sub is_default { 1 }

bless { };
