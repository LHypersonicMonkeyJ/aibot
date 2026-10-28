# Host build of the face renderer: PNG previews and unit tests, no ESP-IDF.
#
#   cmake -S firmware/components/display/host -B firmware/build-host
#   cmake --build firmware/build-host
#   ctest --test-dir firmware/build-host
#   firmware/build-host/face_preview previews/
cmake_minimum_required(VERSION 3.16)
project(face_host CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(face STATIC ../face.cpp)
target_include_directories(face PUBLIC ../include)
target_compile_options(face PRIVATE -Wall -Wextra -Werror)

add_executable(face_preview face_preview.cpp png.cpp)
target_link_libraries(face_preview face)

add_executable(face_test face_test.cpp)
target_link_libraries(face_test face)

enable_testing()
add_test(NAME face_test COMMAND face_test)
