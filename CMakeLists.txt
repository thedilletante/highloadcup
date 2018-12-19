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

set(YUAREL_REPOSITORY https://github.com/jacketizer/libyuarel.git)
set(YUAREL_VERSION 1.0)

set(SQLITECPP_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git)
set(SQLITECPP_VERSION 2.2.0)

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


FetchContent_Declare(
  rapid_json
  GIT_REPOSITORY ${RAPIDJSON_REPOSITORY}
  GIT_TAG        ${RAPIDJSON_VERSION}
)

FetchContent_GetProperties(rapid_json)
if(NOT rapid_json_POPULATED)
  FetchContent_Populate(rapid_json)
  add_subdirectory(${rapid_json_SOURCE_DIR} ${rapid_json_BINARY_DIR})
endif()

FetchContent_Declare(
  yuarel
  GIT_REPOSITORY ${YUAREL_REPOSITORY}
  GIT_TAG        ${YUAREL_VERSION}
)

FetchContent_GetProperties(yuarel)
if(NOT yuarel_POPULATED)
  FetchContent_Populate(yuarel)

  add_library(yuarel
    ${yuarel_SOURCE_DIR}/yuarel.c
    ${yuarel_SOURCE_DIR}/yuarel.h
  )

  target_include_directories(yuarel
    PUBLIC
      ${yuarel_SOURCE_DIR}
  )
endif()

FetchContent_Declare(
  sqlite_cpp
  GIT_REPOSITORY ${SQLITECPP_REPOSITORY}
  GIT_TAG        ${SQLITECPP_VERSION}
)

FetchContent_GetProperties(sqlite_cpp)
if(NOT sqlite_cpp_POPULATED)
  FetchContent_Populate(sqlite_cpp)
  add_subdirectory(${sqlite_cpp_SOURCE_DIR} ${sqlite_cpp_BINARY_DIR})
endif()

add_executable(highloadcup
        src/db.cpp
        src/main.cpp)

find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)

target_link_libraries(highloadcup
  PRIVATE
    GSL
    httpxx
    Threads::Threads
    Boost::system
    Boost::thread
    yuarel
    SQLiteCpp
    sqlite3
    ${CMAKE_DL_LIBS}
)


target_include_directories(highloadcup
  PRIVATE
    ${httpxx_SOURCE_DIR}
    ${rapid_json_SOURCE_DIR}/include
    src
)