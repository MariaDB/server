#
# fix the #line directives in the generated .cc files
# to refer to the original sql_yacc.yy
#
m4_define([b4_syncline],
[b4_sync_start([$1], m4_bpatsubst([$2],[yy_[a-z]+\.yy],sql_yacc.yy))[]dnl

])

# try both paths for different bison versions
m4_sinclude(skeletons/c-skel.m4)
m4_sinclude(c-skel.m4)

