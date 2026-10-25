cmake_minimum_required(VERSION 3.20)
project(firecracker_minifs VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)

option(BUILD_TESTING "Build tests" ON)
option(WITH_NEBULA "Build the MiniFS adapter, backend and tools (needs NebulaR)" ON)

add_compile_options(-Wall -Wextra -Wshadow)

# Storage-independent core: mapping, async disk, vhost-user server.
add_library(remote_disk_core STATIC
  src/disk_mapping.cc src/remote_disk.cc src/vhost_user_blk.cc)
target_include_directories(remote_disk_core PUBLIC include)

if(WITH_NEBULA)
  find_package(NebulaDdl3 REQUIRED)
  add_library(remote_disk_minifs STATIC src/minifs_adapter.cc)
  target_link_libraries(remote_disk_minifs PUBLIC remote_disk_core NebulaR::ddl3)

  add_executable(remote-disk-backend src/main.cc)
  target_link_libraries(remote-disk-backend PRIVATE remote_disk_minifs)

  add_executable(minifs-populate tools/minifs_populate.cc)
  target_link_libraries(minifs-populate PRIVATE remote_disk_minifs)
  install(TARGETS remote-disk-backend minifs-populate RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(unit_tests tests/disk_mapping_test.cc tests/remote_disk_test.cc
                 tests/vhost_user_blk_test.cc)
  target_link_libraries(unit_tests PRIVATE remote_disk_core GTest::gtest_main Threads::Threads)
  add_test(NAME unit_tests COMMAND unit_tests)

  if(WITH_NEBULA)
    add_executable(minifs_integration_test tests/integration/minifs_integration_test.cc)
    target_link_libraries(minifs_integration_test PRIVATE remote_disk_minifs GTest::gtest_main)
    # Needs a live MiniFS target; run through scripts/integration_test.sh.
  endif()
endif()
