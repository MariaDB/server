#ifndef OPT_GROUP_BY_CARDINALITY
#define OPT_GROUP_BY_CARDINALITY


void infer_derived_key_statistics(st_select_lex_unit* derived,
                          KEY *keyinfo, 
                          uint key_parts);



#endif
