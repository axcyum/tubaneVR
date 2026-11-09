# Header-only library shared by driver, sandbox and tools.
add_library(tubane_common INTERFACE)
add_library(tubane::common ALIAS tubane_common)

target_include_directories(tubane_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(tubane_common INTERFACE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE)
