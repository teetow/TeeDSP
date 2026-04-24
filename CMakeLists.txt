cmake_minimum_required(VERSION 3.21)

project(TeeDSP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(DSP_CORE_SOURCES
	src/dsp/Compressor.cpp
	src/dsp/Exciter.cpp
	src/dsp/ParametricEQ.cpp
	src/dsp/ProcessorChain.cpp
)

set(DSP_UI_SOURCES
	src/dsp/ApoSharedClient.cpp
	src/dsp/DspController.cpp
)

set(APO_SOURCES
	src/apo/ApoFactory.cpp
	src/apo/ApoGuidsImpl.cpp
	src/apo/TeeDspApo.cpp
)

set(TEE_DSP_APO_BASE_LIB "" CACHE FILEPATH "Optional full path to audioenginebaseapo.lib")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_WDK_ARCH x64)
else()
	set(_WDK_ARCH x86)
endif()

set(_APO_BASE_LIB_HINTS)
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
	list(APPEND _APO_BASE_LIB_HINTS
		"C:/Program Files (x86)/Windows Kits/10/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um/${_WDK_ARCH}"
	)
endif()

file(GLOB _WDK_UM_LIB_DIRS
	"C:/Program Files (x86)/Windows Kits/10/Lib/*/um/${_WDK_ARCH}"
)
list(APPEND _APO_BASE_LIB_HINTS ${_WDK_UM_LIB_DIRS})

if(TEE_DSP_APO_BASE_LIB)
	set(APO_BASE_LIB "${TEE_DSP_APO_BASE_LIB}")
else()
	find_library(APO_BASE_LIB
		NAMES audioenginebaseapo
		HINTS ${_APO_BASE_LIB_HINTS}
	)
endif()

if(APO_BASE_LIB)
	add_library(TeeDspApo SHARED
		${APO_SOURCES}
		src/apo/TeeDspApo.def
		${DSP_CORE_SOURCES}
	)

	target_include_directories(TeeDspApo PRIVATE
		src
		src/apo
		src/dsp
	)

	target_compile_definitions(TeeDspApo PRIVATE
		UNICODE
		_UNICODE
		NOMINMAX
		WIN32_LEAN_AND_MEAN
	)

	target_link_libraries(TeeDspApo PRIVATE
		${APO_BASE_LIB}
		Avrt
		Ksuser
		Ole32
		Propsys
		Uuid
	)

	if(MSVC)
		target_link_options(TeeDspApo PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/apo/TeeDspApo.def)
	endif()

	set_target_properties(TeeDspApo PROPERTIES
		OUTPUT_NAME "TeeDspApo"
	)
else()
	message(WARNING "audioenginebaseapo.lib not found; skipping TeeDspApo target. Install the Windows Driver Kit and/or set TEE_DSP_APO_BASE_LIB to the full path of audioenginebaseapo.lib.")
endif()

add_executable(TeeDspConfig
	src/ui/main.cpp
	src/ui/MainWindow.cpp
	src/ui/SystemOutputController.cpp
	${DSP_CORE_SOURCES}
	${DSP_UI_SOURCES}
)

target_include_directories(TeeDspConfig PRIVATE
	src
	src/dsp
	src/ui
)

target_compile_definitions(TeeDspConfig PRIVATE
	UNICODE
	_UNICODE
	NOMINMAX
	WIN32_LEAN_AND_MEAN
)

target_link_libraries(TeeDspConfig PRIVATE
	Qt6::Core
	Qt6::Gui
	Qt6::Widgets
)

if(TARGET TeeDspApo)
	install(TARGETS TeeDspApo TeeDspConfig
		RUNTIME DESTINATION .
		LIBRARY DESTINATION .
	)
else()
	install(TARGETS TeeDspConfig
		RUNTIME DESTINATION .
		LIBRARY DESTINATION .
	)
endif()
