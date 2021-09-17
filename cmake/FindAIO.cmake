
SET(AIO_LIBRARY aio)

IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  IF(NOT IGNORE_AIO_CHECK)
    # Ensure aio is available on Linux (required by InnoDB)
    CHECK_INCLUDE_FILES(libaio.h HAVE_LIBAIO_H)
    CHECK_LIBRARY_EXISTS(${AIO_LIBRARY} io_queue_init "" HAVE_LIBAIO)
    IF(NOT HAVE_LIBAIO_H OR NOT HAVE_LIBAIO)
      UNSET(HAVE_LIBAIO_H CACHE)
      UNSET(HAVE_LIBAIO CACHE)
      MESSAGE(FATAL_ERROR "
         aio is required on Linux, you need to install the required library:

           Debian/Ubuntu:              apt-get install libaio-dev
           RedHat/Fedora/Oracle Linux: yum install libaio-devel
           SuSE:                       zypper install libaio-devel

           If you really do not want it, pass -DIGNORE_AIO_CHECK=ON to cmake.
         ")
    ENDIF()

    IF(USE_STATIC_LIBS)
      FIND_STATIC(AIO_STATIC ${AIO_LIBRARY})
      IF(AIO_STATIC)
        SET(AIO_LIBRARY ${AIO_STATIC})
      ENDIF()
    ENDIF()

    # Unfortunately, linking shared libmysqld with static aio
    # does not work,  unless we add also dynamic one. This also means
    # libmysqld.so will depend on libaio.so
    #SET(LIBMYSQLD_SO_EXTRA_LIBS aio)
  ENDIF()
ENDIF()
