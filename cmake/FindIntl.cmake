# cmake/FindIntl.cmake — locate the GNU gettext runtime (libintl) and define
# the `Intl::Intl` imported target. The system library is used, never vendored.
#
# gettext's message functions live in the C library on glibc systems (nothing
# to link); on macOS (Homebrew `brew install gettext`) and Windows they live
# in a separate `intl` library. The prefix to search is an explicit
# -DGETTEXT_ROOT=... when one is given, else the prefix of FindGettext's tool
# (…/gettext/bin/msgfmt), which hints toward the matching runtime.
#
# Result variables: Intl_FOUND, INTL_INCLUDE_DIR, INTL_LIBRARY (empty when the
# C library provides gettext). Imported target: Intl::Intl.
#
# Configure with -DGETTEXT_ROOT=/path (or -DCMAKE_PREFIX_PATH=/path) when the
# runtime lives outside the default search paths.

find_package(Gettext QUIET)

set(_intl_prefixes "")
if(GETTEXT_ROOT)
  list(APPEND _intl_prefixes "${GETTEXT_ROOT}")
endif()
if(GETTEXT_MSGFMT_EXECUTABLE)
  get_filename_component(_intl_bin_dir "${GETTEXT_MSGFMT_EXECUTABLE}" DIRECTORY)
  get_filename_component(_intl_tool_prefix "${_intl_bin_dir}" DIRECTORY)
  list(APPEND _intl_prefixes "${_intl_tool_prefix}")
endif()

find_path(INTL_INCLUDE_DIR NAMES libintl.h
  HINTS ${_intl_prefixes} PATH_SUFFIXES include
  DOC "Path to libintl.h")

find_library(INTL_LIBRARY NAMES intl libintl
  HINTS ${_intl_prefixes} PATH_SUFFIXES lib lib64
  DOC "Path to the gettext runtime library (empty when libc provides gettext)")

# find_path and find_library leave the literal "<VAR>-NOTFOUND" behind, and that
# string is truthy: every `if(VAR)` test below would take it for a real path.
if(INTL_INCLUDE_DIR MATCHES "-NOTFOUND$")
  set(INTL_INCLUDE_DIR "")
endif()
if(INTL_LIBRARY MATCHES "-NOTFOUND$")
  set(INTL_LIBRARY "")
endif()

# glibc ships gettext in libc: a program that includes libintl.h and calls
# gettext() links without any extra library. check_symbol_exists does a full
# compile+link with only the default libraries, so it is true exactly there.
include(CheckSymbolExists)
set(_intl_saved_includes "${CMAKE_REQUIRED_INCLUDES}")
if(INTTL_INCLUDE_DIR)
  list(APPEND CMAKE_REQUIRED_INCLUDES "${INTL_INCLUDE_DIR}")
endif()
check_symbol_exists(gettext "libintl.h" INTL_GETTEXT_IN_LIBC)
set(CMAKE_REQUIRED_INCLUDES "${_intl_saved_includes}")

# Away from glibc the runtime is a separate library, and half of it is useless:
# without the header every translation unit that includes libintl.h fails to
# compile, without the library the link fails.
#
# message(FATAL_ERROR) rather than the usual "set(Intl_FOUND FALSE) and return":
# CMake does not turn a module's <Name>_FOUND FALSE into a configure error, so
# the caller sails on and the first symptom is a generate-time "Intl::Intl
# target not found" naming no package and no remedy. That is not a hypothetical:
# it is how the macOS CI leg failed, at `#include <libintl.h>`, because the
# gettext prefix had not reached the executable. The server cannot run without
# a gettext runtime here anyway, and REQUIRED already says so, so the module
# states it outright. (find_package_handle_standard_args is the one mechanism
# that would report it, but its message names an internal variable instead of
# the missing half.)
if(NOT INTL_GETTEXT_IN_LIBC AND (NOT INTL_INCLUDE_DIR OR NOT INTL_LIBRARY))
  set(_intl_missing "")
  if(NOT INTL_INCLUDE_DIR)
    list(APPEND _intl_missing "libintl.h")
  endif()
  if(NOT INTL_LIBRARY)
    list(APPEND _intl_missing "the intl library")
  endif()
  message(FATAL_ERROR
      "Intl: gettext() is not in the C library here, so the gettext runtime is "
      "required, and this is missing: ${_intl_missing}. Install the runtime "
      "and point CMake at its prefix with -DGETTEXT_ROOT=<prefix> (or "
      "-DCMAKE_PREFIX_PATH=<prefix>): `brew install gettext` on macOS, a "
      "gettext runtime package on Linux (glibc needs none of this, its "
      "libintl.h being in /usr/include), and the plain plus -dev-msvc "
      "bundles of mlocati/gettext-iconv-windows on Windows.")
endif()

set(Intl_FOUND TRUE)
set(INTL_LIBRARIES "${INTL_LIBRARY}")

add_library(Intl::Intl INTERFACE IMPORTED)
set_target_properties(Intl::Intl PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${INTL_INCLUDE_DIR}")
if(INTL_LIBRARY)
  set_property(TARGET Intl::Intl APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES "${INTL_LIBRARY}")
endif()

mark_as_advanced(INTL_INCLUDE_DIR INTL_LIBRARY)
