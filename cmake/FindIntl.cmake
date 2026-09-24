# cmake/FindIntl.cmake — locate the GNU gettext runtime (libintl) and define
# the `Intl::Intl` imported target. The system library is used, never vendored.
#
# gettext's message functions live in the C library on glibc systems (nothing
# to link); on macOS (Homebrew `brew install gettext`) and Windows they live
# in a separate `intl` library. When FindGettext ran first, its tool prefix
# (…/gettext/bin/msgfmt) hints the search toward the matching runtime.
#
# Result variables: Intl_FOUND, INTL_INCLUDE_DIR, INTL_LIBRARY (empty when the
# C library provides gettext). Imported target: Intl::Intl.
#
# Configure with -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/gettext (macOS) or
# -DGETTEXT_ROOT=/path (Windows) when the runtime lives outside the default
# search paths.

find_package(Gettext QUIET)

if(GETTEXT_MSGFMT_EXECUTABLE)
  get_filename_component(_intl_bin_dir "${GETTEXT_MSGFMT_EXECUTABLE}" DIRECTORY)
  get_filename_component(_intl_prefix "${_intl_bin_dir}" DIRECTORY)
else()
  set(_intl_prefix "")
endif()

find_path(INTL_INCLUDE_DIR NAMES libintl.h
  HINTS "${_intl_prefix}/include"
  DOC "Path to libintl.h")

find_library(INTL_LIBRARY NAMES intl libintl
  HINTS "${_intl_prefix}/lib" "${_intl_prefix}/lib64"
  DOC "Path to the gettext runtime library (empty when libc provides gettext)")

# glibc ships gettext in libc: a program that includes libintl.h and calls
# gettext() links without any extra library. check_symbol_exists does a full
# compile+link with only the default libraries, so it is true exactly there.
include(CheckSymbolExists)
set(_intl_saved_includes "${CMAKE_REQUIRED_INCLUDES}")
if(INTL_INCLUDE_DIR)
  list(APPEND CMAKE_REQUIRED_INCLUDES "${INTL_INCLUDE_DIR}")
endif()
check_symbol_exists(gettext "libintl.h" INTL_GETTEXT_IN_LIBC)
set(CMAKE_REQUIRED_INCLUDES "${_intl_saved_includes}")

if(NOT INTL_INCLUDE_DIR AND NOT INTL_LIBRARY AND NOT INTL_GETTEXT_IN_LIBC)
  set(Intl_FOUND FALSE)
  set(Intl_NOT_FOUND_MESSAGE
      "libintl.h was not found and glibc does not provide gettext. Install "
      "the gettext runtime: e.g. `brew install gettext` (macOS; pass "
      "-DCMAKE_PREFIX_PATH=\$(brew --prefix gettext)), a gettext runtime "
      "package for your distribution (Linux), or the MSYS2 gettext runtime "
      "(Windows). Set GETTEXT_ROOT to the prefix if it is not on the default "
      "search paths.")
  return()
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