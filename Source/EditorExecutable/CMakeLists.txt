file(GLOB_RECURSE EDITOR_EXECUTABLE_SOURCES "Private/*.cpp" "Private/*.h")

if(WIN32)
    list(APPEND EDITOR_EXECUTABLE_SOURCES Shalltear.rc)
endif()

add_executable(ShalltearEditorExecutable ${EDITOR_EXECUTABLE_SOURCES})
shalltear_add_executable_manifest(ShalltearEditorExecutable)

set_target_properties(ShalltearEditorExecutable PROPERTIES OUTPUT_NAME Shalltear)

target_link_libraries(ShalltearEditorExecutable PRIVATE ShalltearEditor)

set_target_properties(ShalltearEditorExecutable PROPERTIES WIN32_EXECUTABLE ON)
if(WIN32 AND NOT MINGW)
    target_link_options(ShalltearEditorExecutable PRIVATE "-Wl,/ENTRY:mainCRTStartup")
endif()
