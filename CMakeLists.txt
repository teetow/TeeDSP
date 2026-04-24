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

set(TEE_DSP_APO_BASE_LIB "" CACHE FILEPATH "Optional full path to APO base library (audiobaseprocessingobject.lib, audioenginebaseapo.lib, or audioeng.lib)")

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
	unset(APO_BASE_LIB CACHE)
	find_library(APO_BASE_LIB
		NAMES AudioBaseProcessingObjectV140
		HINTS ${_APO_BASE_LIB_HINTS}
	)
	if(NOT APO_BASE_LIB)
		find_library(APO_BASE_LIB
		NAMES audiobaseprocessingobject
		HINTS ${_APO_BASE_LIB_HINTS}
		)
	endif()
	if(NOT APO_BASE_LIB)
		find_library(APO_BASE_LIB
		NAMES audioenginebaseapo
		HINTS ${_APO_BASE_LIB_HINTS}
		)
	endif()
	if(NOT APO_BASE_LIB)
		find_library(APO_BASE_LIB
			NAMES audioeng
			HINTS ${_APO_BASE_LIB_HINTS}
		)
	endif()
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
		audiomediatypecrt
		Avrt
		Ksuser
		Mmdevapi
		Ole32
		Propsys
		Uuid
		Wmcodecdspuuid
	)

	if(MSVC)
		# Some environments (especially mixed VS instances) do not expose ATL libs
		# on the default linker path even when the component is installed.
		# Resolve atls.lib explicitly and link it by full path when found.
		file(GLOB _ATL_LIB_CANDIDATES
			"C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/atlmfc/lib/${_WDK_ARCH}/atls.lib"
			"C:/Program Files (x86)/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/atlmfc/lib/${_WDK_ARCH}/atls.lib"
		)
		list(LENGTH _ATL_LIB_CANDIDATES _ATL_COUNT)
		if(_ATL_COUNT GREATER 0)
			list(GET _ATL_LIB_CANDIDATES 0 ATLS_LIB)
		endif()
		if(ATLS_LIB)
			get_filename_component(_ATLS_LIB_DIR "${ATLS_LIB}" DIRECTORY)
			target_link_directories(TeeDspApo PRIVATE "${_ATLS_LIB_DIR}")
		else()
			message(WARNING "ATL lib directory not found; TeeDspApo link may fail if dependencies request atls.lib.")
		endif()

		target_link_options(TeeDspApo PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/apo/TeeDspApo.def)
	endif()

	set_target_properties(TeeDspApo PROPERTIES
		OUTPUT_NAME "TeeDspApo"
	)
else()
	message(WARNING "APO base library not found (AudioBaseProcessingObjectV140.lib/audiobaseprocessingobject.lib/audioenginebaseapo.lib/audioeng.lib); skipping TeeDspApo target. Install WDK/SDK libs and/or set TEE_DSP_APO_BASE_LIB to the full path.")
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
