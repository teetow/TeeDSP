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

target_link_libraries(TeeDspApo PRIVATE
    Ole32
    OleAut32
    Uuid
    Propsys
    Advapi32
    RuntimeObject
)

set_target_properties(TeeDspApo PROPERTIES
    OUTPUT_NAME "TeeDspApo"
    PREFIX ""
    SUFFIX ".dll"
)
