cmake_minimum_required(VERSION 3.14)
project(TorModulePlugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

find_package(OpenSSL REQUIRED)

logos_module(
    NAME tor
    LINK_LIBRARIES OpenSSL::Crypto
    SOURCES
        src/tor_impl.h  src/tor_impl.cpp
        src/tor_client.h src/tor_client.cpp
    INCLUDE_DIRS src
)
