cmake_minimum_required(VERSION 3.21)
project(APITester LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# raylib for a lightweight native GUI layer across desktop platforms.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RSHAPES ON CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RTEXTURES ON CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RTEXT ON CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
)
FetchContent_MakeAvailable(raylib)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

if(NOT WIN32)
  find_package(CURL REQUIRED)
endif()

add_executable(api_tester
  src/main.cpp
  src/audit_engine.cpp
  src/report_writer.cpp
)

target_include_directories(api_tester PRIVATE src)
target_link_libraries(api_tester PRIVATE raylib nlohmann_json::nlohmann_json)

if(NOT WIN32)
  target_link_libraries(api_tester PRIVATE CURL::libcurl)
endif()

if(APPLE)
  target_link_libraries(api_tester PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
elseif(UNIX)
  target_link_libraries(api_tester PRIVATE m dl pthread)
elseif(WIN32)
  target_link_libraries(api_tester PRIVATE ws2_32 crypt32 winhttp)
endif()

if(MSVC)
  target_compile_options(api_tester PRIVATE /W4 /permissive-)
else()
  target_compile_options(api_tester PRIVATE -Wall -Wextra -Wpedantic)
endif()
