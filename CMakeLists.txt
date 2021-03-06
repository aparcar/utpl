cmake_minimum_required(VERSION 2.6)
include(CheckFunctionExists)
include(CheckSymbolExists)

PROJECT(utpl C)
ADD_DEFINITIONS(-Os -Wall -Werror --std=gnu99 -Wmissing-declarations -Wno-error=unused-variable -ffunction-sections -D_GNU_SOURCE)

OPTION(FS_SUPPORT "Filesystem plugin support" ON)
OPTION(MATH_SUPPORT "Math plugin support" ON)
OPTION(UBUS_SUPPORT "Ubus plugin support" ON)
OPTION(UCI_SUPPORT "UCI plugin support" ON)

SET(LIB_SEARCH_PATH "/usr/lib/utpl/*.so:/usr/share/utpl/*.utpl:./*.so:./*.utpl" CACHE STRING "Default library search path")
ADD_DEFINITIONS(-DLIB_SEARCH_PATH="${LIB_SEARCH_PATH}")

IF(NOT APPLE)
  SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "-Wl,--gc-sections")
ENDIF()

find_library(json NAMES json-c json)

IF(DEBUG)
  ADD_DEFINITIONS(-DDEBUG -g3 -O0)
ELSE()
  ADD_DEFINITIONS(-DNDEBUG)
ENDIF()

INCLUDE(FindPkgConfig)
PKG_CHECK_MODULES(JSONC json-c json)
IF(JSONC_FOUND)
  ADD_DEFINITIONS(-DJSONC)
  INCLUDE_DIRECTORIES(${JSONC_INCLUDE_DIRS})
ENDIF()

ADD_CUSTOM_COMMAND(
  OUTPUT contrib/lemon
  DEPENDS contrib/lemon.c contrib/lempar.c
  COMMAND gcc -o contrib/lemon contrib/lemon.c
  COMMENT "Generating lemon parser generator"
)

ADD_CUSTOM_COMMAND(
  OUTPUT parser.c
  DEPENDS parser.y contrib/lemon
  COMMAND ./contrib/lemon parser.y
  COMMENT "Generating parser.c"
)

SET_PROPERTY(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES "lemon;parser.h;parser.out")
SET_SOURCE_FILES_PROPERTIES("parser.c" PROPERTIES GENERATED TRUE COMPILE_FLAGS -Wno-error=unused-but-set-variable)
ADD_EXECUTABLE(utpl main.c ast.c lexer.c parser.c eval.c lib.c)
TARGET_LINK_LIBRARIES(utpl ${json})

CHECK_FUNCTION_EXISTS(dlopen DLOPEN_FUNCTION_EXISTS)
IF (NOT DLOPEN_FUNCTION_EXISTS)
  TARGET_LINK_LIBRARIES(utpl dl)
ENDIF()

SET(CMAKE_REQUIRED_LIBRARIES json-c)
CHECK_SYMBOL_EXISTS(json_object_array_shrink "json.h" HAVE_ARRAY_SHRINK)
IF(HAVE_ARRAY_SHRINK)
  ADD_DEFINITIONS(-DHAVE_ARRAY_SHRINK)
ELSE()
  CHECK_SYMBOL_EXISTS(json_object_array_shrink "json-c/json.h" HAVE_ARRAY_SHRINK2)
  IF(HAVE_ARRAY_SHRINK2)
    ADD_DEFINITIONS(-DHAVE_ARRAY_SHRINK)
  ENDIF()
ENDIF()
UNSET(CMAKE_REQUIRED_LIBRARIES)

SET(LIBRARIES "")

IF(FS_SUPPORT)
  SET(LIBRARIES ${LIBRARIES} fs_lib)
  ADD_LIBRARY(fs_lib MODULE lib/fs.c)
  SET_TARGET_PROPERTIES(fs_lib PROPERTIES OUTPUT_NAME lib/fs PREFIX "")
ENDIF()

IF(MATH_SUPPORT)
  SET(LIBRARIES ${LIBRARIES} math_lib)
  ADD_LIBRARY(math_lib MODULE lib/math.c)
  SET_TARGET_PROPERTIES(math_lib PROPERTIES OUTPUT_NAME lib/math PREFIX "")
  CHECK_FUNCTION_EXISTS(ceil CEIL_FUNCTION_EXISTS)
  IF (NOT CEIL_FUNCTION_EXISTS)
    TARGET_LINK_LIBRARIES(math_lib m)
  ENDIF()
ENDIF()

IF(UBUS_SUPPORT)
  SET(LIBRARIES ${LIBRARIES} ubus_lib)
  ADD_LIBRARY(ubus_lib MODULE lib/ubus.c)
  SET_TARGET_PROPERTIES(ubus_lib PROPERTIES OUTPUT_NAME lib/ubus PREFIX "")
  TARGET_LINK_LIBRARIES(ubus_lib ubus blobmsg_json)
ENDIF()

IF(UCI_SUPPORT)
  SET(LIBRARIES ${LIBRARIES} uci_lib)
  ADD_LIBRARY(uci_lib MODULE lib/uci.c)
  SET_TARGET_PROPERTIES(uci_lib PROPERTIES OUTPUT_NAME lib/uci PREFIX "")
  TARGET_LINK_LIBRARIES(uci_lib uci)
ENDIF()

INSTALL(TARGETS utpl RUNTIME DESTINATION bin)
