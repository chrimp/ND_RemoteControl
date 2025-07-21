cmake_minimum_required(VERSION 3.12)

file(GLOB AUDIONDSESSION_SOURCES src/*.cpp)
file(GLOB AUDIONDSESSION_HEADERS include/*.hpp)

# Create a library from MyNDSession
add_library(AudioNDSession STATIC ${AUDIONDSESSION_SOURCES})

# Set C++20 for this library
set_property(TARGET AudioNDSession PROPERTY CXX_STANDARD 20)
set_property(TARGET AudioNDSession PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(AudioNDSession 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../NDSession/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (WIN32)
    target_link_libraries(AudioNDSession
        PUBLIC 
            NetworkDirect
        PRIVATE
            ws2_32
    )
endif()

# Set compile definitions if needed
target_compile_definitions(AudioNDSession PRIVATE
    WIN32_LEAN_AND_MEAN
)