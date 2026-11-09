# tubane_shm_probe — console tool to inspect / drive the shared memory
# without the sandbox GUI (Step 1 verification).
add_executable(tubane_shm_probe main.cpp)
target_link_libraries(tubane_shm_probe PRIVATE tubane::common)
if(MSVC)
    target_compile_options(tubane_shm_probe PRIVATE /W4 /permissive- /utf-8)
endif()
