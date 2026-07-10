ADD_EXECUTABLE(import_util-t import_util-t.cc)
TARGET_LINK_LIBRARIES(import_util-t PRIVATE import_util mytap)
ADD_TEST(import_util import_util-t)
