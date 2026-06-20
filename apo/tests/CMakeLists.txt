add_executable(TeeDspApoProbe
    ApoLoadProbe.cpp
)

target_compile_features(TeeDspApoProbe PRIVATE cxx_std_20)

target_compile_definitions(TeeDspApoProbe PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_include_directories(TeeDspApoProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(TeeDspApoProbe PRIVATE
    Mmdevapi
    Ole32
)
