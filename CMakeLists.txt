cmake_minimum_required(VERSION 3.6.0)
project(FactoryPlanningTools VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g")

list(APPEND CMAKE_CXX_FLAGS "-g -Wall -Werror -Wsign-compare")

set(CMAKE_CXX_FLAGS_ASAN "-g -fsanitize=address,undefined -fno-sanitize-recover=all -O2"
    CACHE STRING "Compiler flags in asan build"
    FORCE)

set(CMAKE_CXX_FLAGS_REL "-O2 -DNDEBUG"
    CACHE STRING "Compiler flags for the release version"
    FORCE)


message("General compiler flags: " ${CMAKE_CXX_FLAGS})
message("Debug build compiler flags: " ${CMAKE_CXX_FLAGS_DEBUG})
message("ASAN build compiler flags: " ${CMAKE_CXX_FLAGS_ASAN})
message("Release build compiler flags: " ${CMAKE_CXX_FLAGS_REL})

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(${PROJECT_NAME} ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_executable(${PROJECT_NAME}.tests ${CMAKE_SOURCE_DIR}/tests/test_main.cpp)

find_package(GTest CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}.tests
    PRIVATE
    GTest::gtest
    GTest::gmock
    GTest::gtest_main
)
