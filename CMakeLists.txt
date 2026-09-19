cmake_minimum_required(VERSION 3.16)
project(MonitoringAgent VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(cpp-httplib)

add_executable(monitoring_agent src/main.cpp)

target_link_libraries(monitoring_agent PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
)

find_package(Threads REQUIRED)
target_link_libraries(monitoring_agent PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(monitoring_agent PRIVATE ws2_32)
endif()