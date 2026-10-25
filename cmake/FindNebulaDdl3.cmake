# Provides the imported target NebulaR::ddl3 (headers + libs + UCX + numa).
#
# Strategy, in order:
#  1. An installed/exported package:  find_package(nebula_common CONFIG)
#     -> Nebula::nebula_ddl3. NebulaR currently cannot produce this (its
#     common/ export fails: nebula_common -> nebula_resiliency is not in the
#     export set), so this path is future-proofing.
#  2. NebulaR_ROOT (headers) + NebulaR_BUILD_DIR (static libs from a NebulaR
#     build with -DNEBULA_ENABLE_UCX=ON). Nothing in NebulaR is modified; the
#     libraries and headers are only read.
if(TARGET NebulaR::ddl3)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx)
find_library(NEBULA_NUMA_LIBRARY numa)

find_package(nebula_common CONFIG QUIET)
if(TARGET Nebula::nebula_ddl3)
  add_library(NebulaR::ddl3 INTERFACE IMPORTED)
  target_link_libraries(NebulaR::ddl3 INTERFACE Nebula::nebula_ddl3)
  message(STATUS "NebulaR ddl3: installed package")
  return()
endif()

if(NOT NebulaR_ROOT)
  set(NebulaR_ROOT "$ENV{HOME}/nebula-ai/repos/NebulaR")
endif()
if(NOT NebulaR_BUILD_DIR)
  set(NebulaR_BUILD_DIR "${NebulaR_ROOT}/build")
endif()

set(_libs "")
foreach(_name nebula_ddl3 nebula_common nebula_resiliency nebula_log nebula_logring)
  find_library(NEBULA_LIB_${_name} NAMES ${_name} PATHS "${NebulaR_BUILD_DIR}/lib"
               NO_DEFAULT_PATH)
  if(NOT NEBULA_LIB_${_name})
    message(FATAL_ERROR
      "lib${_name} not found in ${NebulaR_BUILD_DIR}/lib. Build NebulaR's "
      "client library out of this project (cmake --build <dir> --target "
      "nebula_ddl3 with NEBULA_ENABLE_UCX=ON) and pass -DNebulaR_BUILD_DIR=<dir>.")
  endif()
  list(APPEND _libs "${NEBULA_LIB_${_name}}")
endforeach()
if(NOT EXISTS "${NebulaR_ROOT}/common/ddl/client_target/core_client.h")
  message(FATAL_ERROR "NebulaR_ROOT=${NebulaR_ROOT} is not a NebulaR checkout")
endif()

add_library(NebulaR::ddl3 INTERFACE IMPORTED)
target_include_directories(NebulaR::ddl3 SYSTEM INTERFACE
  "${NebulaR_ROOT}/common" "${NebulaR_ROOT}" "${NebulaR_ROOT}/common/util/include"
  "${NebulaR_ROOT}/common/core/include" "${NebulaR_ROOT}/target/resiliency")
target_compile_definitions(NebulaR::ddl3 INTERFACE NEBULA_UCX_ENABLED=1)
# Static archives have circular references (ddl3 <-> common <-> resiliency).
target_link_libraries(NebulaR::ddl3 INTERFACE
  "-Wl,--start-group" ${_libs} "-Wl,--end-group" PkgConfig::UCX Threads::Threads)
if(NEBULA_NUMA_LIBRARY)
  target_compile_definitions(NebulaR::ddl3 INTERFACE ECLIB_HAS_NUMA=1)
  target_link_libraries(NebulaR::ddl3 INTERFACE ${NEBULA_NUMA_LIBRARY})
endif()
message(STATUS "NebulaR ddl3: build tree ${NebulaR_BUILD_DIR}")
