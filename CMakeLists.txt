cmake_minimum_required(VERSION 3.21)

project(TeeDSP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
# CLAP SDK (header-only). Fetched at configure time, pinned to a tag so the
# build is reproducible. Exposes an INTERFACE target named `clap`.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(clap
	GIT_REPOSITORY https://github.com/free-audio/clap.git
	GIT_TAG 1.2.2
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clap)

# ---------------------------------------------------------------------------
# teedsp_dsp — the Qt-free DSP core. Linked only by the CLAP plugin now; the
# app no longer runs DSP inline (it hosts teedsp.clap). DspController is not
# here: it's a QObject param model and belongs to the host/UI side.
# ---------------------------------------------------------------------------
add_library(teedsp_dsp STATIC
	src/dsp/Compressor.cpp
	src/dsp/Exciter.cpp
	src/dsp/Leveler.cpp
	src/dsp/SpectralLeveler.cpp
	src/dsp/ParametricEQ.cpp
	src/dsp/ProcessorChain.cpp
)
target_compile_features(teedsp_dsp PUBLIC cxx_std_20)
target_include_directories(teedsp_dsp PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/src/dsp
)
target_compile_definitions(teedsp_dsp PRIVATE NOMINMAX)

set(DSP_SOURCES
	src/dsp/DspController.cpp
)

# APO-era editor: no capture/render engine, no in-app CLAP host. Only device
# enumeration (for the picker), the FFT analyzer (fed from the APO sample ring),
# and the APO shared-memory client remain.
set(HOST_SOURCES
	src/host/WasapiDevices.cpp
	src/host/Fft.cpp
	src/host/SpectrumAnalyzer.cpp
	src/host/ApoSharedClient.cpp
)

set(UI_SOURCES
	src/ui/main.cpp
	src/ui/MainWindow.cpp
	src/ui/Theme.cpp
	src/ui/StartupRegistration.cpp
	src/ui/AudioServiceRecovery.cpp
	src/ui/TrayController.cpp
	src/ui/widgets/Knob.cpp
	src/ui/widgets/LevelMeter.cpp
	src/ui/widgets/SpectralGainMeter.cpp
	src/ui/widgets/EqCurve.cpp
	src/ui/widgets/BipolarGainMeter.cpp
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
	clap
	Qt6::Core
	Qt6::Gui
	Qt6::Widgets
	Avrt
	Mmdevapi
	Ole32
	Propsys
	Shell32
	Uuid
	Winmm
)

# Keep theme.qss next to the built exe so the stylesheet loads when running
# straight from the build tree (Theme.cpp looks in applicationDirPath in
# non-Debug builds). Install copies it too, below.
add_custom_command(TARGET TeeDsp POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/src/ui/theme.qss"
		"$<TARGET_FILE_DIR:TeeDsp>/theme.qss"
	COMMENT "Copying theme.qss next to TeeDsp.exe"
	VERBATIM)

install(TARGETS TeeDsp
	RUNTIME DESTINATION .
)

install(FILES
	src/ui/theme.qss
	DESTINATION .
)

option(TEEDSP_BUILD_APO "Build the TeeDSP Audio Processing Object DLL" ON)
if(TEEDSP_BUILD_APO)
	add_subdirectory(apo)
endif()

option(TEEDSP_BUILD_PLUGIN "Build the TeeDSP CLAP plugin (the whole DSP chain)" ON)
if(TEEDSP_BUILD_PLUGIN)
	add_subdirectory(plugin)
endif()
