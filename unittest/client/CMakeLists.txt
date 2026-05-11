ADD_EXECUTABLE(import_util-t import_util-t.cc)
TARGET_LINK_LIBRARIES(import_util-t PRIVATE import_util mytap)
ADD_TEST(import_util import_util-t)

ADD_EXECUTABLE(mysql_translate-t mysql_translate-t.cc)
TARGET_LINK_LIBRARIES(mysql_translate-t PRIVATE mysql_translate mytap mysys strings dbug)
ADD_TEST(mysql_translate mysql_translate-t)
