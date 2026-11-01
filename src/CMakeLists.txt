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

target_include_directories(main PRIVATE
    "${CMAKE_SOURCE_DIR}/ui/windows/runner"
)
target_include_directories(main_service PRIVATE
    "${CMAKE_SOURCE_DIR}/ui/windows/runner"
)

target_link_libraries(main PRIVATE flutter_launcher)
target_link_libraries(main_service PRIVATE flutter_launcher)

function(copy_flutter_runtime TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${TARGET}>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FLUTTER_LIBRARY}"
            "$<TARGET_FILE_DIR:${TARGET}>/flutter_windows.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FLUTTER_ICU_DATA_FILE}"
            "$<TARGET_FILE_DIR:${TARGET}>/data/icudtl.dat"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_BUILD_DIR}/flutter_assets"
            "$<TARGET_FILE_DIR:${TARGET}>/data/flutter_assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_BUILD_DIR}/native_assets/windows"
            "$<TARGET_FILE_DIR:${TARGET}>"
    )
endfunction()

copy_flutter_runtime(main)
copy_flutter_runtime(main_service)