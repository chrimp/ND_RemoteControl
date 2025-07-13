cmake_minimum_required(VERSION 3.12)

file(GLOB D2DPRESENTATION_SOURCES src/*.cpp)
file(GLOB D2DPRESENTATION_HEADERS include/*.hpp)

# Create a library from MyNDSession
add_library(D2DPresentation STATIC ${D2DPRESENTATION_SOURCES})

# Set C++20 for this library
set_property(TARGET D2DPresentation PROPERTY CXX_STANDARD 20)
set_property(TARGET D2DPresentation PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(D2DPresentation 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set compile definitions if needed
target_compile_definitions(D2DPresentation PRIVATE
    WIN32_LEAN_AND_MEAN
)