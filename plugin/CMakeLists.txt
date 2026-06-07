# TeeDSP CLAP plugin — the whole DSP chain as a single .clap module.
#
# Loadable module (LoadLibrary on Windows) with the `.clap` extension. Links
# the Qt-free teedsp_dsp core and the header-only CLAP SDK. No Qt.

add_library(teedsp_clap MODULE
    TeeDspPlugin.cpp
)

target_compile_features(teedsp_clap PRIVATE cxx_std_20)

target_include_directories(teedsp_clap PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(teedsp_clap PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(teedsp_clap PRIVATE
    teedsp_dsp
    clap
)

# Match the host's MSVC runtime (dynamic UCRT) so the module loads in-process.
set_property(TARGET teedsp_clap PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set_target_properties(teedsp_clap PROPERTIES
    OUTPUT_NAME "teedsp"
    PREFIX ""
    SUFFIX ".clap"
)

# The app loads the plugin from next to its own exe — build it alongside the
# app and drop a fresh copy next to TeeDsp.exe after every plugin build.
if(TARGET TeeDsp)
    add_dependencies(TeeDsp teedsp_clap)
    add_custom_command(TARGET teedsp_clap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:teedsp_clap> $<TARGET_FILE_DIR:TeeDsp>
        COMMENT "Copying teedsp.clap next to TeeDsp.exe")
endif()

install(TARGETS teedsp_clap
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)
