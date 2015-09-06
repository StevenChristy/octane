project(OctaneTest)
cmake_minimum_required(VERSION 3.1.0 FATAL_ERROR)
set(CMAKE_CXX_STANDARD 14)
set(SRC_FILES test.cpp)
add_executable(testx ${SRC_FILES})
target_link_libraries(testx pthread)

