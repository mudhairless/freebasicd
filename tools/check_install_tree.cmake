# Check the tree `cmake --install` produced. Run it the way CI does:
#
#   cmake --install build --config Release --prefix install-prefix
#   cmake -DFBLANG_INSTALL_PREFIX=install-prefix \
#         -DFBLANG_EXPECT_CATALOGS=29 -P tools/check_install_tree.cmake
#
# The install rules are the only release-shaped thing this project has, and
# nothing else tests them: a broken DESTINATION or a renamed domain is
# invisible to ctest. This script is the test, and it is a `cmake -P` script
# rather than shell so the same check runs on Linux, macOS, and Windows.
#
# Required: FBLANG_INSTALL_PREFIX (relative paths resolve against the current
# working directory, which is the repository root in CI).
# Optional: FBLANG_EXPECT_CATALOGS (0 or unset means "do not count them" — set
# it to 0 for a leg that deliberately builds without gettext),
#           FBLANG_BIN_SUBDIR (defaults to GNUInstallDirs' `bin`).
#
# Any missing piece is a FATAL_ERROR, so a failure exits non-zero.

cmake_minimum_required(VERSION 3.16)

if(NOT FBLANG_INSTALL_PREFIX)
  message(FATAL_ERROR "FBLANG_INSTALL_PREFIX is not set; pass "
                      "-DFBLANG_INSTALL_PREFIX=<dir>")
endif()
get_filename_component(_prefix "${FBLANG_INSTALL_PREFIX}" ABSOLUTE
                       BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${_prefix}")
  message(FATAL_ERROR "install prefix does not exist: ${_prefix}")
endif()

set(_bindir "${FBLANG_BIN_SUBDIR}")
if(NOT _bindir)
  set(_bindir bin)
endif()

set(_problems "")

# The server binary.
set(_exe "")
foreach(_name freebasicd freebasicd.exe)
  if(EXISTS "${_prefix}/${_bindir}/${_name}")
    set(_exe "${_prefix}/${_bindir}/${_name}")
  endif()
endforeach()
if(NOT _exe)
  list(APPEND _problems "no freebasicd binary in ${_prefix}/${_bindir}/")
endif()

# The GPL requires the license to travel with the binary. GNUInstallDirs puts
# it under share/doc/<project>, so glob for it rather than spelling the path.
file(GLOB_RECURSE _licenses "${_prefix}/share/doc/LICENSE.md"
                         "${_prefix}/share/doc/*/LICENSE.md")
list(LENGTH _licenses _license_count)
if(_license_count EQUAL 0)
  list(APPEND _problems "no LICENSE.md under ${_prefix}/share/doc/")
endif()

# The message catalogs, one per po/<lang>.po, all named after the domain. An
# empty count against a non-zero expectation is the "gettext was missing, so
# the build quietly shipped English-only" failure, and it must be loud.
file(GLOB _catalogs "${_prefix}/share/locale/*/LC_MESSAGES/freebasicd.mo")
list(LENGTH _catalogs _catalog_count)
if(FBLANG_EXPECT_CATALOGS)
  list(LENGTH FBLANG_EXPECT_CATALOGS _expected_length)
  if(_expected_length EQUAL 0)
    set(_expected 0)
  else()
    set(_expected "${FBLANG_EXPECT_CATALOGS}")
  endif()
  if(NOT _catalog_count EQUAL _expected)
    list(APPEND _problems
         "found ${_catalog_count} message catalogs, expected ${_expected} "
         "(is gettext installed?)")
  endif()
else()
  set(_expected "unchecked")
endif()
foreach(_mo IN LISTS _catalogs)
  file(SIZE "${_mo}" _size)
  if(_size EQUAL 0)
    list(APPEND _problems "empty message catalog: ${_mo}")
  endif()
endforeach()

message(STATUS "install prefix: ${_prefix}")
if(_exe)
  message(STATUS "binary:         ${_exe}")
endif()
message(STATUS "license copies:  ${_license_count}")
message(STATUS "catalogs:        ${_catalog_count} (expected ${_expected})")

if(_problems)
  foreach(_problem IN LISTS _problems)
    message(FATAL_ERROR "${_problem}")
  endforeach()
endif()
message(STATUS "install tree OK")
