ADD_EXECUTABLE(import_util-t import_util-t.cc)
TARGET_LINK_LIBRARIES(import_util-t PRIVATE import_util mytap)
ADD_TEST(import_util import_util-t)

ADD_EXECUTABLE(mysql8_translate-t mysql8_translate-t.cc)
TARGET_LINK_LIBRARIES(mysql8_translate-t PRIVATE mysql8_translate mytap mysys strings dbug)
ADD_TEST(mysql8_translate mysql8_translate-t)
