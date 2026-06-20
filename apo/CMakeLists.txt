# TeeDSP APO — Mode Effect (post-mix) audio processing object.
#
# Loaded into audiodg.exe by the Windows audio engine when registered
# against an endpoint's MFX slot. Built as a separate DLL with no Qt
# dependency — only the Windows SDK.
#
# Stage 1: pass-through. No DSP, just proves the COM/registration path.
# Stage 3: ProcessorChain plugged into APOProcess.

if(NOT WIN32)
    message(STATUS "TeeDspApo: skipped (Windows-only)")
    return()
endif()

add_library(TeeDspApo SHARED
    Guids.h
    TeeDspApo.h
    TeeDspApo.cpp
    DllMain.cpp
    TeeDspApo.def
)

target_compile_features(TeeDspApo PRIVATE cxx_std_20)

target_compile_definitions(TeeDspApo PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# audiodg.exe links the dynamic UCRT — match it.
set_property(TARGET TeeDspApo PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# An embedded manifest makes the loader touch APIs that are forbidden inside
# the protected audiodg.exe process, so the APO fails to load there even when
# correctly signed. Suppress manifest embedding entirely.
target_link_options(TeeDspApo PRIVATE /MANIFEST:NO)

target_link_libraries(TeeDspApo PRIVATE
    teedsp_dsp          # Qt-free DSP core: ProcessorChain + processors
    Ole32
    OleAut32
    Uuid
    Propsys
    Advapi32
    AudioBaseProcessingObject
    legacy_stdio_definitions
    RuntimeObject
)

set_target_properties(TeeDspApo PROPERTIES
    OUTPUT_NAME "TeeDspApo"
    PREFIX ""
    SUFFIX ".dll"
)

option(TEEDSP_BUILD_APO_PROBE "Build the temporary direct-render APO load probe" OFF)
if(TEEDSP_BUILD_APO_PROBE)
    add_subdirectory(tests)
endif()
