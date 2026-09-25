cmake_minimum_required(VERSION 3.13)
project(es2netif-bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_executable(es2netif-bridge
  src/main.cpp
  src/config.cpp
  src/event_server.cpp
  src/json.cpp
  src/log.cpp
  src/netif_session.cpp
)
target_include_directories(es2netif-bridge PRIVATE src)
# Vendored Epson headers are kept verbatim, so their warnings are suppressed.
target_include_directories(es2netif-bridge SYSTEM PRIVATE vendor/epson)
target_compile_definitions(es2netif-bridge PRIVATE _GNU_SOURCE)
target_compile_options(es2netif-bridge PRIVATE -Wall -Wextra)
target_link_libraries(es2netif-bridge PRIVATE Threads::Threads)

install(TARGETS es2netif-bridge DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES es2netif-bridge.conf.example DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES systemd/es2netif-bridge.service DESTINATION ${CMAKE_INSTALL_DOCDIR})

option(ES2NB_BUILD_TESTS "Build the fake es2netif test double" ON)
if(ES2NB_BUILD_TESTS)
  add_executable(fake_netif tests/fake_netif.cpp)
  target_include_directories(fake_netif SYSTEM PRIVATE vendor/epson)
  target_compile_options(fake_netif PRIVATE -Wall -Wextra)

  enable_testing()
  add_test(NAME end_to_end
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(end_to_end PROPERTIES TIMEOUT 300)
endif()
