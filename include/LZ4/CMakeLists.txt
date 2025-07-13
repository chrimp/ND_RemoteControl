cmake_minimum_required(VERSION 3.12)

add_library(LZ4 INTERFACE)

file(GLOB LZ4_HEADERS
    include/*.h
)

target_sources(LZ4 INTERFACE ${LZ4_HEADERS})

# Set C++20 for this library
set_property(TARGET LZ4 PROPERTY CXX_STANDARD 20)
set_property(TARGET LZ4 PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(LZ4 
    INTERFACE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Set compile definitions if needed
target_compile_definitions(LZ4 INTERFACE
    WIN32_LEAN_AND_MEAN
)

target_compile_definitions(LZ4 INTERFACE LZ4="${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_link_directories(LZ4 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib)