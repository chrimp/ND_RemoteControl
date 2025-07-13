cmake_minimum_required(VERSION 3.12)

add_library(DirectXTex INTERFACE)

file(GLOB DIRECTXTEX_HEADERS
    include/*.h
    include/*.inl
)

target_sources(DirectXTex INTERFACE ${DIRECTXTEX_HEADERS})

# Set C++20 for this library
set_property(TARGET DirectXTex PROPERTY CXX_STANDARD 20)
set_property(TARGET DirectXTex PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(DirectXTex 
    INTERFACE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Set compile definitions if needed
target_compile_definitions(DirectXTex INTERFACE
    WIN32_LEAN_AND_MEAN
)

target_compile_definitions(DirectXTex INTERFACE DIRECTXTEX_LIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lib")