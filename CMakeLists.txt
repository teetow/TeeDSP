cmake_minimum_required(VERSION 3.21)

project(TeeDSP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

set(DSP_SOURCES
	src/dsp/Compressor.cpp
	src/dsp/Exciter.cpp
	src/dsp/Leveler.cpp
	src/dsp/ParametricEQ.cpp
	src/dsp/ProcessorChain.cpp
	src/dsp/DspController.cpp
)

set(HOST_SOURCES
	src/host/WasapiDevices.cpp
	src/host/WasapiLoopbackCapture.cpp
	src/host/WasapiRender.cpp
	src/host/WasapiDeviceNotifier.cpp
	src/host/AudioEngine.cpp
	src/host/Fft.cpp
	src/host/Resampler.cpp
	src/host/SpectrumAnalyzer.cpp
)

set(UI_SOURCES
	src/ui/main.cpp
	src/ui/MainWindow.cpp
	src/ui/Theme.cpp
	src/ui/StartupRegistration.cpp
	src/ui/TrayController.cpp
	src/ui/widgets/Knob.cpp
	src/ui/widgets/LevelMeter.cpp
	src/ui/widgets/EqCurve.cpp
)

add_executable(TeeDsp WIN32
	${UI_SOURCES}
	${DSP_SOURCES}
	${HOST_SOURCES}
	src/ui/app.rc
)

target_include_directories(TeeDsp PRIVATE
	src
)

target_compile_definitions(TeeDsp PRIVATE
	UNICODE
	_UNICODE
	NOMINMAX
	WIN32_LEAN_AND_MEAN
	$<$<CONFIG:Debug>:TEEDSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}">
)

target_link_libraries(TeeDsp PRIVATE
	Qt6::Core
	Qt6::Gui
	Qt6::Widgets
	Qt6::OpenGL
	Qt6::OpenGLWidgets
	Avrt
	Mmdevapi
	Ole32
	Propsys
	Uuid
	Winmm
)

install(TARGETS TeeDsp
	RUNTIME DESTINATION .
)

install(FILES
	src/ui/theme.qss
	DESTINATION .
)
