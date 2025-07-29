add_executable(main main.cpp)
add_executable(main_service main.cpp)
add_executable(service service.cpp)

if (WIN32)
    target_link_libraries(main PRIVATE DesktopDuplication NDSession NetworkDirect D2DPresentation InputNDSession AudioNDSession)
    target_link_libraries(main_service PRIVATE DesktopDuplication_service NDSession NetworkDirect D2DPresentation InputNDSession AudioNDSession)

    set_target_properties(main_service PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
    )
endif()

