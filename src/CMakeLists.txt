add_executable(main main.cpp)

if (WIN32)
    target_link_libraries(main PRIVATE DesktopDuplication NDSession NetworkDirect)
endif()