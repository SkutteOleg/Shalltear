file(GLOB_RECURSE BENCHMARK_SOURCES "Private/*.cpp" "Private/*.h")

add_executable(ShalltearBenchmarks ${BENCHMARK_SOURCES})
shalltear_add_executable_manifest(ShalltearBenchmarks)

target_link_libraries(ShalltearBenchmarks PRIVATE ShalltearCore)
