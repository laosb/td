# BLAH: build-time datacenter configuration.
#
# Upstream TDLib hardcodes Telegram's datacenter addresses and RSA public keys.
# A Blah build takes both from a C3 control plane at *configure* time: the
# endpoint URL is read from blah-server.config.url in the repository root (the
# same convention the iOS and web clients use), the JSON is downloaded, and it
# is embedded verbatim into a generated header that BlahDcConfig.h parses once
# at runtime.
#
# With nothing configured the generated header carries an empty document, every
# call site falls through to upstream behaviour, and the build is a stock TDLib.
#
# Inputs, first one that yields a document wins:
#   -DBLAH_DC_CONFIG_FILE=<path>  a C3 /api/dcs response already on disk
#   -DBLAH_DC_CONFIG_URL=<url>    fetch this URL instead of the file below
#   blah-server.config.url        the checked-in endpoint, a single line
#
# A configured source that cannot be read or does not describe a datacenter
# fails the build: shipping a client that silently talks to real Telegram
# servers is worse than not shipping one.

set(BLAH_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(BLAH_GENERATED_DIR "${CMAKE_BINARY_DIR}/blah")
set(BLAH_GENERATED_HEADER "${BLAH_GENERATED_DIR}/td/telegram/net/BlahDcConfigData.h")
set(BLAH_URL_FILE "${BLAH_ROOT_DIR}/blah-server.config.url")

set(BLAH_DC_CONFIG_JSON "")
set(BLAH_DC_CONFIG_SOURCE "")

if (BLAH_DC_CONFIG_FILE)
  if (NOT EXISTS "${BLAH_DC_CONFIG_FILE}")
    message(FATAL_ERROR "BLAH_DC_CONFIG_FILE does not exist: ${BLAH_DC_CONFIG_FILE}")
  endif()
  file(READ "${BLAH_DC_CONFIG_FILE}" BLAH_DC_CONFIG_JSON)
  set(BLAH_DC_CONFIG_SOURCE "${BLAH_DC_CONFIG_FILE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BLAH_DC_CONFIG_FILE}")
else()
  if (NOT BLAH_DC_CONFIG_URL AND EXISTS "${BLAH_URL_FILE}")
    file(READ "${BLAH_URL_FILE}" BLAH_DC_CONFIG_URL)
    string(STRIP "${BLAH_DC_CONFIG_URL}" BLAH_DC_CONFIG_URL)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BLAH_URL_FILE}")
  endif()
  if (BLAH_DC_CONFIG_URL)
    set(BLAH_DOWNLOAD_PATH "${BLAH_GENERATED_DIR}/dcs.json")
    message(STATUS "BLAH: fetching the datacenter list from ${BLAH_DC_CONFIG_URL}")
    file(DOWNLOAD "${BLAH_DC_CONFIG_URL}" "${BLAH_DOWNLOAD_PATH}"
         STATUS BLAH_DOWNLOAD_STATUS TIMEOUT 30 TLS_VERIFY ON)
    list(GET BLAH_DOWNLOAD_STATUS 0 BLAH_DOWNLOAD_CODE)
    if (NOT BLAH_DOWNLOAD_CODE EQUAL 0)
      list(GET BLAH_DOWNLOAD_STATUS 1 BLAH_DOWNLOAD_MESSAGE)
      message(FATAL_ERROR "BLAH: failed to fetch ${BLAH_DC_CONFIG_URL}: ${BLAH_DOWNLOAD_MESSAGE}")
    endif()
    file(READ "${BLAH_DOWNLOAD_PATH}" BLAH_DC_CONFIG_JSON)
    set(BLAH_DC_CONFIG_SOURCE "${BLAH_DC_CONFIG_URL}")
  endif()
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
