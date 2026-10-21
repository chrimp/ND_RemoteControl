add_executable(port_handover port_handover.cpp)

if (WIN32)
    target_link_libraries(port_handover PRIVATE DesktopDuplication NDSession NetworkDirect D2DPresentation InputNDSession AudioNDSession)

endif()

