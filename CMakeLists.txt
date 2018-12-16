cmake_minimum_required(VERSION 3.12)
project(highloadcup)
include(FetchContent)

set(CMAKE_CXX_STANDARD 11)

set(GSL_REPOSITORY https://github.com/Microsoft/GSL.git)
set(GSL_VERSION v2.0.0)

set(HTTPXX_REPOSITORY https://github.com/AndreLouisCaron/httpxx.git)
set(HTTPXX_VERSION master)

set(RAPIDJSON_REPOSITORY https://github.com/Tencent/rapidjson.git)
set(RAPIDJSON_VERSION v1.1.0)

FetchContent_Declare(
  gsl
  GIT_REPOSITORY ${GSL_REPOSITORY}
  GIT_TAG        ${GSL_VERSION}
)

FetchContent_GetProperties(gsl)
if(NOT gsl_POPULATED)
  FetchContent_Populate(gsl)
  add_subdirectory(${gsl_SOURCE_DIR} ${gsl_BINARY_DIR})
endif()

FetchContent_Declare(
  httpxx
  GIT_REPOSITORY ${HTTPXX_REPOSITORY}
  GIT_TAG        ${HTTPXX_VERSION}
)

FetchContent_GetProperties(httpxx)
if(NOT httpxx_POPULATED)
  FetchContent_Populate(httpxx)
  add_subdirectory(${httpxx_SOURCE_DIR} ${httpxx_BINARY_DIR})
endif()

#
#FetchContent_Declare(
#  rapid_json
#  GIT_REPOSITORY ${RAPIDJSON_REPOSITORY}
#  GIT_TAG        ${RAPIDJSON_VERSION}
#)
#
#FetchContent_GetProperties(rapid_json)
#if(NOT rapid_json_POPULATED)
#  FetchContent_Populate(rapid_json)
#  add_subdirectory(${rapid_json_SOURCE_DIR} ${rapid_json_BINARY_DIR})
#endif()

add_executable(highloadcup main.cpp)

#find_package(RapidJSON REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)

target_link_libraries(highloadcup
  PRIVATE
    GSL
    httpxx
    Threads::Threads
    Boost::system
    Boost::thread
)


target_include_directories(highloadcup
  PRIVATE
    ${httpxx_SOURCE_DIR}
#    ${rapid_json_SOURCE_DIR}/include
)