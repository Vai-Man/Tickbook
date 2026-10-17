cmake_minimum_required(VERSION 3.14)
project(tickbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(tickbook
  src/itch.cpp
  src/order_book.cpp
  src/matching_engine.cpp
  src/fill_model.cpp
  src/backtest.cpp
  src/synth.cpp)
target_include_directories(tickbook PUBLIC include)
if(MSVC)
  target_compile_options(tickbook PRIVATE /W4)
else()
  target_compile_options(tickbook PRIVATE -Wall -Wextra)
endif()

add_executable(tickbook_cli src/main.cpp)
set_target_properties(tickbook_cli PROPERTIES OUTPUT_NAME tickbook)
target_link_libraries(tickbook_cli PRIVATE tickbook)

enable_testing()
add_executable(tickbook_tests tests/test_tickbook.cpp)
target_link_libraries(tickbook_tests PRIVATE tickbook)
add_test(NAME tickbook_tests COMMAND tickbook_tests)
