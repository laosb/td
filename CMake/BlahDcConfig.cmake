# BLAH: explicit application-owned DC bootstrap. No network access occurs
# while configuring the build. An omitted document preserves stock TDLib;
# applications may instead provide BLAH_DC_CONFIG JSON before creating clients.

set(BLAH_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(BLAH_GENERATED_DIR "${CMAKE_BINARY_DIR}/blah")
set(BLAH_GENERATED_HEADER "${BLAH_GENERATED_DIR}/td/telegram/net/BlahDcConfigData.h")

set(BLAH_DC_CONFIG_JSON "")
set(BLAH_DC_CONFIG_SOURCE "")

if (BLAH_DC_CONFIG_FILE)
  if (NOT EXISTS "${BLAH_DC_CONFIG_FILE}")
    message(FATAL_ERROR "BLAH_DC_CONFIG_FILE does not exist: ${BLAH_DC_CONFIG_FILE}")
  endif()
  file(READ "${BLAH_DC_CONFIG_FILE}" BLAH_DC_CONFIG_JSON)
  set(BLAH_DC_CONFIG_SOURCE "${BLAH_DC_CONFIG_FILE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BLAH_DC_CONFIG_FILE}")
endif()

if (BLAH_DC_CONFIG_SOURCE)
  # Not a JSON parser, just the assertion that the response describes at least
  # one datacenter with a key. BlahDcConfig.h does the real parsing.
  if (NOT BLAH_DC_CONFIG_JSON MATCHES "\"rsaPublicKey\"")
    message(FATAL_ERROR "BLAH: ${BLAH_DC_CONFIG_SOURCE} advertised no datacenter with an RSA public key")
  endif()
  # The raw-string delimiter below has to be absent from the payload. A JSON
  # document cannot contain it unescaped, so this only catches corruption.
  if (BLAH_DC_CONFIG_JSON MATCHES "\\)BLAHDC")
    message(FATAL_ERROR "BLAH: ${BLAH_DC_CONFIG_SOURCE} contains the raw-string delimiter )BLAHDC")
  endif()
  message(STATUS "BLAH: datacenter configuration taken from ${BLAH_DC_CONFIG_SOURCE}")
else()
  message(STATUS "BLAH: no datacenter configuration; building stock TDLib")
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/BlahDcConfigData.h.in" "${BLAH_GENERATED_HEADER}" @ONLY)

# Every target below this point compiles against the generated header, which is
# why this include has to sit near the top of the root CMakeLists.txt.
include_directories("${BLAH_GENERATED_DIR}")
