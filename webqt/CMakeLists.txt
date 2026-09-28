cmake_minimum_required(VERSION 3.21)
project(TeeDSPWeb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_add_executable(teedsp-editor main.cpp Client.cpp MainWindowRemote.cpp
    ../src/dsp/DspController.cpp ../src/ui/MainWindow.cpp
    ../src/ui/widgets/Knob.cpp ../src/ui/widgets/EqCurve.cpp
    ../src/ui/widgets/LevelMeter.cpp ../src/ui/widgets/BipolarGainMeter.cpp
    ../src/ui/widgets/SpectralGainMeter.cpp ../src/ui/widgets/SectionBox.cpp)
target_include_directories(teedsp-editor PRIVATE . ../src)
target_compile_definitions(teedsp-editor PRIVATE TEEDSP_REMOTE)
target_link_libraries(teedsp-editor PRIVATE Qt6::Widgets Qt6::Network)
qt_add_resources(teedsp-editor theme PREFIX "/" BASE ../src/ui FILES ../src/ui/theme.qss)
if(EMSCRIPTEN)
    target_link_options(teedsp-editor PRIVATE "-sALLOW_MEMORY_GROWTH=1")
endif()
