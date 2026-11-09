# tubane_vr_check - end-to-end check through SteamVR: writes controller input
# and poses into shared memory, reads them back as an OpenVR application.
add_executable(tubane_vr_check main.cpp)
target_link_libraries(tubane_vr_check PRIVATE tubane::common tubane::openvr_api)
# Shares the sandbox's header-only velocity estimator.
target_include_directories(tubane_vr_check PRIVATE "${PROJECT_SOURCE_DIR}/sandbox/src")
if(MSVC)
    target_compile_options(tubane_vr_check PRIVATE /W4 /permissive- /utf-8)
endif()

# openvr_api.dll + action manifest / bindings next to the exe.
add_custom_command(TARGET tubane_vr_check POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:openvr_api>" "$<TARGET_FILE_DIR:tubane_vr_check>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/actions.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/bindings_knuckles.json"
            "$<TARGET_FILE_DIR:tubane_vr_check>")
