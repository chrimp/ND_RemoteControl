cmake_minimum_required (VERSION 3.12)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add source to this project's executable.
project (remoteControl CXX)

# Set a custom output directory for the final executable.
# CMake will create this directory automatically.
# The $<CONFIG> generator expression will resolve to "Debug", "Release", etc.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/bin/$<CONFIG>")

if (WIN32)
    add_subdirectory("include/NetworkDirect")
    add_subdirectory("include/NDSession")
    add_subdirectory("include/DesktopDuplication")
    add_subdirectory("include/D2DPresentation")
    add_subdirectory("include/DirectXTex")
    add_subdirectory("src")
endif()