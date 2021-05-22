#
# fix the #line directives in the generated .cc files
# to refer to the original sql_yacc.yy
#
m4_define([yyfile],m4_bpatsubst(__file__,[[a-z.0-9]+$],sql_yacc.yy))

m4_define([b4_syncline],
[m4_if(m4_index([$2],[.yy]),[-1],
[b4_sync_start([$1], [$2])[]dnl

],[b4_sync_start([$1], ["yyfile"])[]dnl

])])

# try both paths for different bison versions
m4_sinclude(skeletons/c-skel.m4)
m4_sinclude(c-skel.m4)

