cmake_minimum_required(VERSION 3.12)

file(GLOB DESKTOPDUPLICATION_SOURCES src/*.cpp)
file(GLOB DESKTOPDUPLICATION_HEADERS include/*.hpp)

# Create a library from MyNDSession
add_library(DesktopDuplication STATIC ${DESKTOPDUPLICATION_SOURCES})

# Set C++20 for this library
set_property(TARGET DesktopDuplication PROPERTY CXX_STANDARD 20)
set_property(TARGET DesktopDuplication PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(DesktopDuplication 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set compile definitions if needed
target_compile_definitions(DesktopDuplication PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)