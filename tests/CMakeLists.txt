add_executable(TeeDspLevelerTests
    LevelerTests.cpp
)

target_link_libraries(TeeDspLevelerTests PRIVATE teedsp_dsp)

add_test(NAME TeeDspLevelerTests COMMAND TeeDspLevelerTests)
