IF(NOT WITHOUT_SERVER)

    ADD_DEFINITIONS(-DFRM_PARSER)

    INCLUDE_DIRECTORIES(
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sql
        ${CMAKE_BINARY_DIR}/sql
        ${CMAKE_BINARY_DIR}/include
        ${LIBFMT_INCLUDE_DIR}
        ${PCRE_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
        ${SSL_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/tpool
        ${CMAKE_SOURCE_DIR}/mysys
        ${CMAKE_SOURCE_DIR}/strings
    )

    IF(WITH_WSREP)
        INCLUDE_DIRECTORIES(
            ${CMAKE_SOURCE_DIR}/wsrep-lib/include
            ${CMAKE_SOURCE_DIR}/wsrep-lib/wsrep-API/v26
        )
    ENDIF()

    SET(FRM_PARSER_SOURCES
            mariadb_frm.cc
            frm_mocks.cc
            ${CMAKE_SOURCE_DIR}/unittest/sql/dummy_builtins.cc
            ${CMAKE_SOURCE_DIR}/sql/table.cc
    )

    MYSQL_ADD_EXECUTABLE(mariadb-frm ${FRM_PARSER_SOURCES})
    TARGET_COMPILE_FEATURES(mariadb-frm PRIVATE cxx_std_20)

    IF(MSVC)
        TARGET_COMPILE_OPTIONS(mariadb-frm PRIVATE
                /wd4100  
                /wd4204  
                /wd4456  
                /Gy      
        )
    ELSE()
        TARGET_COMPILE_OPTIONS(mariadb-frm PRIVATE
                -Wno-missing-field-initializers
                -Wno-unused-label
        )
    ENDIF()

    TARGET_COMPILE_DEFINITIONS(mariadb-frm PRIVATE
            MYSQL_SERVER
    )

    TARGET_LINK_LIBRARIES(mariadb-frm
        sql_builtins
        mysys
        strings
        dbug
        mysys_ssl
        sql
        ${ZLIB_LIBRARIES}
        ${SSL_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
    )

    IF(MSVC)
        TARGET_LINK_OPTIONS(mariadb-frm PRIVATE
                /INCREMENTAL:NO
                /FORCE:MULTIPLE
                /IGNORE:4006
                /IGNORE:4088
                /IGNORE:4217
                /WX:NO
        )
    ELSEIF(APPLE)
        TARGET_COMPILE_OPTIONS(mariadb-frm PRIVATE
                -fno-common  
        )
        SET_TARGET_PROPERTIES(mariadb-frm PROPERTIES
                LINK_FLAGS "-Wl,-w -Wl,-no_pie -Wl,-dead_strip -mmacosx-version-min=14.0"
        )
    ELSE()
        TARGET_LINK_OPTIONS(mariadb-frm PRIVATE -flto
                "LINKER:--allow-multiple-definition"
                "-ffunction-sections"
                "LINKER:--gc-sections")
    ENDIF()

    ADD_DEPENDENCIES(mariadb-frm GenError)

ENDIF(NOT WITHOUT_SERVER)