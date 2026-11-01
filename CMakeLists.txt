cmake_minimum_required (VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add source to this project's executable.
project (remoteControl CXX)

#add_subdirectory(ui)

# Set a custom output directory for the final executable.
# CMake will create this directory automatically.
# The $<CONFIG> generator expression will resolve to "Debug", "Release", etc.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/bin/$<CONFIG>")

if (MSVC)
    add_compile_options(/wd4596)
endif()

if (WIN32)
    add_subdirectory("include/NetworkDirect")
    add_subdirectory("include/NDSession")
    add_subdirectory("include/DesktopDuplication")
    add_subdirectory("include/D2DPresentation")
    add_subdirectory("include/InputNDSession")
    add_subdirectory("include/AudioNDSession")
    #add_subdirectory("include/TestSession")
    add_subdirectory("ui/windows")
    add_subdirectory("src")
    add_subdirectory("tests")
endif()