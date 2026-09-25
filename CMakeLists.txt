cmake_minimum_required(VERSION 3.13)
project(es-bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_executable(es-bridge
  src/main.cpp
  src/config.cpp
  src/event_server.cpp
  src/json.cpp
  src/log.cpp
  src/netif_session.cpp
)
target_include_directories(es-bridge PRIVATE src)
# Vendored Epson headers are kept verbatim, so their warnings are suppressed.
target_include_directories(es-bridge SYSTEM PRIVATE vendor/epson)
target_compile_definitions(es-bridge PRIVATE _GNU_SOURCE)
target_compile_options(es-bridge PRIVATE -Wall -Wextra)
target_link_libraries(es-bridge PRIVATE Threads::Threads)

install(TARGETS es-bridge DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES es-bridge.conf.example DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES systemd/es-bridge.service DESTINATION ${CMAKE_INSTALL_DOCDIR})

option(ESB_BUILD_TESTS "Build the fake es2netif test double" ON)
if(ESB_BUILD_TESTS)
  add_executable(fake_netif tests/fake_netif.cpp)
  target_include_directories(fake_netif SYSTEM PRIVATE vendor/epson)
  target_compile_options(fake_netif PRIVATE -Wall -Wextra)

  enable_testing()
  add_test(NAME end_to_end
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(end_to_end PROPERTIES TIMEOUT 300)
endif()
