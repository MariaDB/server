
MACRO(MAKE_WIX_IDENTIFIER str varname)
  STRING(REPLACE "/" "." ${varname} "${str}")
  STRING(REGEX REPLACE "[^a-zA-Z_0-9.]" "_" ${varname} "${${varname}}")
  STRING(LENGTH "${${varname}}" len)
  # Identifier should be smaller than 72 character
  # We have to cut down the length to 70 chars, since we add 2 char prefix
  # pretty often
  IF(len GREATER 70)
     MATH(EXPR diff "${len}-67")
     STRING(SUBSTRING "${${varname}}" ${diff} 67 shortstr)
     SET(${varname} "___${shortstr}")
  ENDIF()
ENDMACRO()

SET($ENV{VS_UNICODE_OUTPUT} "")

FOREACH(third_party ${WITH_THIRD_PARTY})
  INCLUDE(${SRCDIR}/${third_party}.cmake)
 
  # Check than above script produced ${third_party}.wxi and ${third_party}_feature.wxi
  FOREACH(outfile ${third_party}.wxi ${third_party}_feature.wxi)
    IF(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${outfile})
      MESSAGE(FATAL_ERROR 
       "${SRCDIR}/${third_party}.cmake did not produce "
       "${CMAKE_CURRENT_BINARY_DIR}/${outfile}"
      )
    ENDIF()
  ENDFOREACH()
ENDFOREACH()


SET(CANDLE_ARCH -arch ${Platform})
IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
  SET(Win64 " Win64='yes'")
  SET(PlatformProgramFilesFolder ProgramFiles64Folder)
  SET(CA_QUIET_EXEC CAQuietExec64)
ELSE()
  SET(PlatformProgramFilesFolder ProgramFilesFolder)
  SET(CA_QUIET_EXEC CAQuietExec)
  SET(Win64)
ENDIF()

SET(ENV{VS_UNICODE_OUTPUT})

INCLUDE(${TOP_BINDIR}/CPackConfig.cmake)

IF(CPACK_WIX_CONFIG)
  INCLUDE(${CPACK_WIX_CONFIG})
ENDIF()

IF(NOT CPACK_WIX_UI)
  SET(CPACK_WIX_UI "MyWixUI_Mondo")
ENDIF()

IF(CMAKE_INSTALL_CONFIG_NAME)
  STRING(REPLACE "${CMAKE_CFG_INTDIR}" "${CMAKE_INSTALL_CONFIG_NAME}" 
    WIXCA_LOCATION "${WIXCA_LOCATION}")
  SET(CONFIG_PARAM "-DCMAKE_INSTALL_CONFIG_NAME=${CMAKE_INSTALL_CONFIG_NAME}")
ENDIF()

SET(COMPONENTS_ALL "${CPACK_COMPONENTS_ALL}")
FOREACH(comp ${COMPONENTS_ALL})
 SET(ENV{DESTDIR} testinstall/${comp})
 EXECUTE_PROCESS(
  COMMAND ${CMAKE_COMMAND} ${CONFIG_PARAM} -DCMAKE_INSTALL_COMPONENT=${comp}  
   -DCMAKE_INSTALL_PREFIX=  -P ${TOP_BINDIR}/cmake_install.cmake
   OUTPUT_QUIET

  )
  # Exclude empty install components
  SET(INCLUDE_THIS_COMPONENT 1)
  SET(MANIFEST_FILENAME "${TOP_BINDIR}/install_manifest_${comp}.txt")
  IF(EXISTS ${MANIFEST_FILENAME})
    FILE(READ ${MANIFEST_FILENAME} content)
    STRING(LENGTH "${content}" content_length)
    IF (content_length EQUAL 0)
      MESSAGE(STATUS "Excluding empty component ${comp}")
      SET(INCLUDE_THIS_COMPONENT 0)
    ENDIF()
  ENDIF()
  IF(NOT INCLUDE_THIS_COMPONENT)
    LIST(REMOVE_ITEM CPACK_COMPONENTS_ALL "${comp}")
  ELSE()
    SET(DIRS ${DIRS} testinstall/${comp})
  ENDIF()
ENDFOREACH()

SET(WIX_FEATURES)
FOREACH(comp ${CPACK_COMPONENTS_ALL})
 STRING(TOUPPER "${comp}" comp_upper)
 IF(NOT CPACK_COMPONENT_${comp_upper}_GROUP)
   SET(WIX_FEATURE_${comp_upper}_COMPONENTS "${comp}")
   SET(CPACK_COMPONENT_${comp_upper}_HIDDEN 1)
   SET(CPACK_COMPONENT_GROUP_${comp_upper}_DISPLAY_NAME 
      ${CPACK_COMPONENT_${comp_upper}_DISPLAY_NAME})
   SET(CPACK_COMPONENT_GROUP_${comp_upper}_DESCRIPTION 
      ${CPACK_COMPONENT_${comp_upper}_DESCRIPTION})
   SET(CPACK_COMPONENT_GROUP_${comp_upper}_WIX_LEVEL
      ${CPACK_COMPONENT_${comp_upper}_WIX_LEVEL})
   SET(WIX_FEATURES ${WIX_FEATURES} WIX_FEATURE_${comp_upper})
 ELSE()
   SET(FEATURE_NAME WIX_FEATURE_${CPACK_COMPONENT_${comp_upper}_GROUP})
   SET(WIX_FEATURES ${WIX_FEATURES} ${FEATURE_NAME})
   LIST(APPEND ${FEATURE_NAME}_COMPONENTS ${comp})
 ENDIF()
ENDFOREACH()

IF(WIX_FEATURES)
  LIST(REMOVE_DUPLICATES WIX_FEATURES)
ENDIF()

SET(CPACK_WIX_FEATURES)

FOREACH(f ${WIX_FEATURES})
 STRING(TOUPPER "${f}" f_upper)
 STRING(REPLACE "WIX_FEATURE_" "" f_upper ${f_upper})
 IF (CPACK_COMPONENT_GROUP_${f_upper}_DISPLAY_NAME)
  SET(TITLE ${CPACK_COMPONENT_GROUP_${f_upper}_DISPLAY_NAME})
 ELSE()
  SET(TITLE  CPACK_COMPONENT_GROUP_${f_upper}_DISPLAY_NAME)
 ENDIF()

 IF (CPACK_COMPONENT_GROUP_${f_upper}_DESCRIPTION)
  SET(DESCRIPTION ${CPACK_COMPONENT_GROUP_${f_upper}_DESCRIPTION})
 ELSE()
  SET(DESCRIPTION CPACK_COMPONENT_GROUP_${f_upper}_DESCRIPTION)
 ENDIF()
 IF(CPACK_COMPONENT_${f_upper}_WIX_LEVEL)
   SET(Level ${CPACK_COMPONENT_${f_upper}_WIX_LEVEL})
 ELSE()
   SET(Level 1)
 ENDIF()
 IF(CPACK_COMPONENT_GROUP_${f_upper}_HIDDEN)
   SET(DISPLAY "Display='hidden'")
   SET(TITLE ${f_upper})
   SET(DESCRIPTION ${f_upper})
 ELSE()
   SET(DISPLAY)
   IF(CPACK_COMPONENT_GROUP_${f_upper}_EXPANDED)
    SET(DISPLAY "Display='expand'")
   ENDIF()
   IF (CPACK_COMPONENT_GROUP_${f_upper}_DISPLAY_NAME)
    SET(TITLE ${CPACK_COMPONENT_GROUP_${f_upper}_DISPLAY_NAME})
   ELSE()
     SET(TITLE  CPACK_COMPONENT_GROUP_${f_upper}_DISPLAY_NAME)
   ENDIF()
   IF (CPACK_COMPONENT_GROUP_${f_upper}_DESCRIPTION)
     SET(DESCRIPTION ${CPACK_COMPONENT_GROUP_${f_upper}_DESCRIPTION})
   ELSE()
     SET(DESCRIPTION CPACK_COMPONENT_GROUP_${f_upper}_DESCRIPTION)
   ENDIF()
 ENDIF()
 
 SET(CPACK_WIX_FEATURES 
 "${CPACK_WIX_FEATURES}
   <Feature  Id='${f_upper}'
     Title='${TITLE}'
     Description='${DESCRIPTION}'
     ConfigurableDirectory='INSTALLDIR'
     AllowAdvertise='no'
     Level='${Level}' ${DISPLAY} >"
  )
 FOREACH(c ${${f}_COMPONENTS})
   
   STRING(TOUPPER "${c}" c_upper)
   IF (CPACK_COMPONENT_${c_upper}_DISPLAY_NAME)
    SET(TITLE ${CPACK_COMPONENT_${c_upper}_DISPLAY_NAME})
   ELSE()
    SET(TITLE CPACK_COMPONENT_${c_upper}_DISPLAY_NAME)
   ENDIF()

   IF (CPACK_COMPONENT_${c_upper}_DESCRIPTION)
     SET(DESCRIPTION ${CPACK_COMPONENT_${c_upper}_DESCRIPTION})
   ELSE()
     SET(DESCRIPTION CPACK_COMPONENT_${c_upper}_DESCRIPTION)
   ENDIF()
   IF(CPACK_COMPONENT_${c_upper}_WIX_LEVEL)
    SET(Level ${CPACK_COMPONENT_${c_upper}_WIX_LEVEL})
   ELSE()
    SET(Level 1)
   ENDIF()
   MAKE_WIX_IDENTIFIER("${c}" cg)
   
   IF(CPACK_COMPONENT_${c_upper}_HIDDEN)
   SET(CPACK_WIX_FEATURES
   "${CPACK_WIX_FEATURES} 
     <ComponentGroupRef Id='componentgroup.${cg}'/>")
   ELSE()
   SET(CPACK_WIX_FEATURES
   "${CPACK_WIX_FEATURES} 
    <Feature Id='${c}' 
       Title='${TITLE}'
       Description='${DESCRIPTION}'
       ConfigurableDirectory='INSTALLDIR'
       AllowAdvertise='no'
       Level='${Level}'>
       <ComponentGroupRef Id='componentgroup.${cg}'/>
    </Feature>")
  ENDIF()
  
  ENDFOREACH()
  IF(${f}_EXTRA_FEATURES)
  FOREACH(extra_feature ${${f}_EXTRA_FEATURES})
    SET(CPACK_WIX_FEATURES
       "${CPACK_WIX_FEATURES} 
       <FeatureRef Id='${extra_feature}' />
	   ")  
  ENDFOREACH()
  ENDIF()
  SET(CPACK_WIX_FEATURES
   "${CPACK_WIX_FEATURES}
   </Feature>
   ")
ENDFOREACH()



MACRO(GENERATE_GUID VarName)
 EXECUTE_PROCESS(COMMAND uuidgen -c 
 OUTPUT_VARIABLE ${VarName}
 OUTPUT_STRIP_TRAILING_WHITESPACE)
ENDMACRO()


FUNCTION(TRAVERSE_FILES dir topdir file file_comp  dir_root)
  FILE(GLOB all_files ${dir}/*)
  IF(NOT all_files)
    RETURN()
  ENDIF()
  FILE(RELATIVE_PATH dir_rel ${topdir} ${dir})
  IF(dir_rel)
   MAKE_DIRECTORY(${dir_root}/${dir_rel})
   MAKE_WIX_IDENTIFIER("${dir_rel}" id)
   SET(DirectoryRefId  "D.${id}")
  ELSE()
   SET(DirectoryRefId "INSTALLDIR")
  ENDIF()
  FILE(APPEND ${file} "<DirectoryRef Id='${DirectoryRefId}'>\n")
 
  SET(NONEXEFILES)
  FOREACH(v MAJOR_VERSION MINOR_VERSION PATCH_VERSION TINY_VERSION)
    IF(NOT DEFINED ${v})
      MESSAGE(FATAL_ERROR "${v} is not defined")
    ENDIF()
  ENDFOREACH()
  SET(default_version "${MAJOR_VERSION}.${MINOR_VERSION}.${PATCH_VERSION}.${TINY_VERSION}")

  FOREACH(f ${all_files})
    IF(NOT IS_DIRECTORY ${f})
      FILE(RELATIVE_PATH rel ${topdir} ${f})
      MAKE_WIX_IDENTIFIER("${rel}" id)
      FILE(TO_NATIVE_PATH ${f} f_native)
      GET_FILENAME_COMPONENT(f_ext "${f}" EXT)
      GET_FILENAME_COMPONENT(name "${f}" NAME)

      IF(name STREQUAL ".empty")
        # Create an empty directory
        GENERATE_GUID(guid)
        FILE(APPEND ${file} "  <Component Id='C.${id}' Guid='${guid}' ${Win64}> <CreateFolder/> </Component>\n")
        FILE(APPEND ${file_comp} "<ComponentRef Id='C.${id}'/>\n")
      ELSEIF(NOT ${file}.COMPONENT_EXCLUDE)
        FILE(APPEND ${file} "  <Component Id='C.${id}' Guid='*' ${Win64} >\n")
        IF(${id}.COMPONENT_CONDITION)
          FILE(APPEND ${file} "    <Condition>${${id}.COMPONENT_CONDITION}</Condition>\n")
        ENDIF()
        FILE(APPEND ${file} "    <File Id='F.${id}' KeyPath='yes' Source='${f_native}'")
        FILE(APPEND ${file} " DefaultVersion='${default_version}' DefaultLanguage='1033'")
        IF(${id}.FILE_EXTRA)
          FILE(APPEND ${file} ">\n${${id}.FILE_EXTRA}</File>")
        ELSE()
          FILE(APPEND ${file} "/>\n")
        ENDIF()
        FILE(APPEND ${file} "  </Component>\n")
        FILE(APPEND ${file_comp} "  <ComponentRef Id='C.${id}'/>\n")
      ENDIF()
    ENDIF()
  ENDFOREACH()
  FILE(APPEND ${file} "</DirectoryRef>\n")
  IF(NONEXEFILES)
    GENERATE_GUID(guid)
    SET(ComponentId "C._files_${COMP_NAME}.${DirectoryRefId}")
    MAKE_WIX_IDENTIFIER("${ComponentId}" ComponentId)
    FILE(APPEND ${file} 
    "<DirectoryRef Id='${DirectoryRefId}'>\n<Component Guid='${guid}'
   Id='${ComponentId}' ${Win64}>${NONEXEFILES}\n</Component></DirectoryRef>\n")
    FILE(APPEND ${file_comp} "  <ComponentRef Id='${ComponentId}'/>\n")
  ENDIF()
  FOREACH(f ${all_files})
    IF(IS_DIRECTORY ${f})
      TRAVERSE_FILES(${f} ${topdir} ${file} ${file_comp}  ${dir_root})
    ENDIF()
  ENDFOREACH()
ENDFUNCTION()

FUNCTION(TRAVERSE_DIRECTORIES dir topdir file prefix)
  FILE(RELATIVE_PATH rel ${topdir} ${dir})
  IF(rel)
    IF (IS_DIRECTORY "${f}")
      MAKE_WIX_IDENTIFIER("${rel}" id)
      GET_FILENAME_COMPONENT(name ${dir} NAME)
      FILE(APPEND ${file} "${prefix}<Directory Id='D.${id}' Name='${name}'>\n")
    ENDIF()
  ENDIF()
  FILE(GLOB all_files ${dir}/*)
    FOREACH(f ${all_files})
    IF(IS_DIRECTORY ${f})
      TRAVERSE_DIRECTORIES(${f} ${topdir} ${file} "${prefix}  ")
    ENDIF()
  ENDFOREACH()
  IF(rel)
    IF(IS_DIRECTORY "${f}")
      FILE(APPEND ${file} "${prefix}</Directory>\n")
    ENDIF()
  ENDIF()
ENDFUNCTION()

SET(CPACK_WIX_COMPONENTS)
SET(CPACK_WIX_COMPONENT_GROUPS)
GET_FILENAME_COMPONENT(abs . ABSOLUTE)

FOREACH(d ${DIRS})
  GET_FILENAME_COMPONENT(d ${d} ABSOLUTE)
  GET_FILENAME_COMPONENT(d_name ${d} NAME)

  MAKE_WIX_IDENTIFIER("${d_name}" d_name)
  FILE(WRITE ${abs}/${d_name}_component_group.wxs 
  "<ComponentGroup Id='componentgroup.${d_name}'>")
  SET(COMP_NAME ${d_name})
  TRAVERSE_FILES(${d} ${d} ${abs}/${d_name}.wxs 
    ${abs}/${d_name}_component_group.wxs "${abs}/dirs")
  FILE(APPEND  ${abs}/${d_name}_component_group.wxs   "</ComponentGroup>")
  IF(EXISTS ${d_name}.wxs)
    FILE(READ ${d_name}.wxs WIX_TMP)
    SET(CPACK_WIX_COMPONENTS "${CPACK_WIX_COMPONENTS}\n${WIX_TMP}")
    FILE(REMOVE ${d_name}.wxs)
  ENDIF()
  
  FILE(READ ${d_name}_component_group.wxs WIX_TMP)
 
  SET(CPACK_WIX_COMPONENT_GROUPS "${CPACK_WIX_COMPONENT_GROUPS}\n${WIX_TMP}")
  FILE(REMOVE ${d_name}_component_group.wxs)
ENDFOREACH()

FILE(WRITE directories.wxs "<DirectoryRef Id='INSTALLDIR'>\n")
TRAVERSE_DIRECTORIES(${abs}/dirs ${abs}/dirs directories.wxs "")
FILE(APPEND directories.wxs "</DirectoryRef>\n")

FILE(READ directories.wxs CPACK_WIX_DIRECTORIES)
FILE(REMOVE directories.wxs)


FOREACH(src ${CPACK_WIX_INCLUDE})
SET(CPACK_WIX_INCLUDES 
"${CPACK_WIX_INCLUDES}
 <?include ${src}?>"
)
ENDFOREACH()


CONFIGURE_FILE(${SRCDIR}/mysql_server.wxs.in
 ${CMAKE_CURRENT_BINARY_DIR}/mysql_server.wxs)
CONFIGURE_FILE(${SRCDIR}/extra.wxs.in
  ${CMAKE_CURRENT_BINARY_DIR}/extra.wxs)

SET(EXTRA_CANDLE_ARGS "$ENV{EXTRA_CANDLE_ARGS}")

SET(EXTRA_LIGHT_ARGS -cc . -reusecab)
IF("$ENV{EXTRA_LIGHT_ARGS}")
  SET(EXTRA_LIGHT_ARGS "$ENV{EXTRA_LIGHT_ARGS}")
ENDIF()

FILE(REMOVE mysql_server.wixobj extra.wixobj)
STRING(REPLACE " " ";" EXTRA_WIX_PREPROCESSOR_FLAGS_LIST ${EXTRA_WIX_PREPROCESSOR_FLAGS})
EXECUTE_PROCESS(
 COMMAND ${CANDLE_EXECUTABLE} 
 ${EXTRA_WIX_PREPROCESSOR_FLAGS_LIST}
 ${CANDLE_ARCH} 
 -ext WixUtilExtension 
 -ext WixFirewallExtension   
 mysql_server.wxs 
 ${EXTRA_CANDLE_ARGS}
)

EXECUTE_PROCESS(
 COMMAND ${CANDLE_EXECUTABLE} ${CANDLE_ARCH}
 ${EXTRA_WIX_PREPROCESSOR_FLAGS_LIST}
 -ext WixUtilExtension 
 -ext WixFirewallExtension  
 ${CMAKE_CURRENT_BINARY_DIR}/extra.wxs 
 ${EXTRA_CANDLE_ARGS}
)

IF(VCRedist_MSM)
  SET(SILENCE_VCREDIST_MSM_WARNINGS  -sice:ICE82 -sice:ICE03)
ENDIF()

EXECUTE_PROCESS(
 COMMAND ${LIGHT_EXECUTABLE} -v -ext WixUIExtension -ext WixUtilExtension
  -ext WixFirewallExtension -sice:ICE61 -sw1103 ${SILENCE_VCREDIST_MSM_WARNINGS}
  mysql_server.wixobj  extra.wixobj -out  ${CPACK_PACKAGE_FILE_NAME}.msi
  ${EXTRA_LIGHT_ARGS}
)

IF(SIGNCODE)
  SEPARATE_ARGUMENTS(SIGNTOOL_PARAMETERS WINDOWS_COMMAND "${SIGNTOOL_PARAMETERS}")
  EXECUTE_PROCESS(
  COMMAND ${SIGNTOOL_EXECUTABLE} sign ${SIGNTOOL_PARAMETERS} 
  /d ${CPACK_PACKAGE_FILE_NAME}.msi
  ${CPACK_PACKAGE_FILE_NAME}.msi
)
ENDIF()
CONFIGURE_FILE(${CPACK_PACKAGE_FILE_NAME}.msi 
 ${TOP_BINDIR}/${CPACK_PACKAGE_FILE_NAME}.msi
  COPYONLY)

