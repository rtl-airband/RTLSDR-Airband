include(CheckCXXCompilerFlag)
include(CheckCXXSymbolExists)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

if(UNIX OR MINGW)
	add_definitions(-D_FILE_OFFSET_BITS=64)
	# isnormal()
	add_definitions(-D_POSIX_C_SOURCE=200112L)
endif()

CHECK_CXX_COMPILER_FLAG(-pthread CXX_HAS_PTHREAD)
if(CXX_HAS_PTHREAD)
    add_compile_options(-pthread)
endif()

CHECK_CXX_COMPILER_FLAG(-ffast-math CXX_HAS_FFAST_MATH)
if(CXX_HAS_FFAST_MATH)
    add_compile_options(-ffast-math)
endif()

# asprintf on MacOS
if(APPLE)
	add_definitions(-D_DARWIN_C_SOURCE)
endif()

# sincosf on linux vs __sincosf on MacOS
set(CMAKE_REQUIRED_DEFINITIONS_ORIG ${CMAKE_REQUIRED_DEFINITIONS})
list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")
set(CMAKE_REQUIRED_LIBRARIES_ORIG ${CMAKE_REQUIRED_LIBRARIES})
list(APPEND CMAKE_REQUIRED_LIBRARIES m)
CHECK_SYMBOL_EXISTS(sincosf math.h HAVE_SINCOSF)
if(HAVE_SINCOSF)
	set(SINCOSF "sincosf")
else()
	CHECK_SYMBOL_EXISTS(__sincosf math.h HAVE___SINCOSF)
	if(HAVE___SINCOSF)
		set(SINCOSF "__sincosf")
	endif()
endif()
if(NOT HAVE_SINCOSF AND NOT HAVE___SINCOSF)
	message(FATAL_ERROR "Required function sincosf() is unavailable")
endif()
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS_ORIG})
set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES_ORIG})

find_library(LIBM m REQUIRED)
find_library(LIBDL dl REQUIRED)
find_library(LIBPTHREAD pthread REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(CONFIG REQUIRED libconfig++)
list(APPEND rtl_airband_extra_libs ${CONFIG_LIBRARIES})
list(APPEND rtl_airband_include_dirs ${CONFIG_INCLUDE_DIRS})
list(APPEND link_dirs ${CONFIG_LIBRARY_DIRS})

# Can't use pkg_check_modules here, as some distros do not install lame.pc file
find_package(Lame REQUIRED)
list(APPEND rtl_airband_extra_libs ${LAME_LIBRARIES})
list(APPEND rtl_airband_include_dirs ${LAME_INCLUDE_DIR})

pkg_check_modules(SHOUT REQUIRED shout)
list(APPEND rtl_airband_extra_libs ${SHOUT_LIBRARIES})
list(APPEND rtl_airband_include_dirs ${SHOUT_INCLUDE_DIRS})
list(APPEND link_dirs ${SHOUT_LIBRARY_DIRS})

set(CMAKE_REQUIRED_INCLUDES_SAVE ${CMAKE_REQUIRED_INCLUDES})
set(CMAKE_REQUIRED_LIBRARIES_SAVE ${CMAKE_REQUIRED_LIBRARIES})
set(CMAKE_REQUIRED_LINK_OPTIONS_SAVE ${CMAKE_REQUIRED_LINK_OPTIONS})
set(CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES} ${SHOUT_INCLUDE_DIRS}")
set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES} ${SHOUT_LIBRARIES}")
if ( NOT "${SHOUT_LIBRARY_DIRS}" STREQUAL "" )
	set(CMAKE_REQUIRED_LINK_OPTIONS "-L${SHOUT_LIBRARY_DIRS}")
endif()
set(LIBSHOUT_HEADER "shout/shout.h")
CHECK_CXX_SYMBOL_EXISTS("SHOUT_TLS_AUTO" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_TLS_AUTO)
CHECK_CXX_SYMBOL_EXISTS("SHOUT_TLS_AUTO_NO_PLAIN" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_TLS_AUTO_NO_PLAIN)
CHECK_CXX_SYMBOL_EXISTS("SHOUT_TLS_RFC2818" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_TLS_RFC2818)
CHECK_CXX_SYMBOL_EXISTS("SHOUT_TLS_RFC2817" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_TLS_RFC2817)
CHECK_CXX_SYMBOL_EXISTS("SHOUT_TLS_DISABLED" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_TLS_DISABLED)
CHECK_CXX_SYMBOL_EXISTS("shout_set_tls" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_SET_TLS)
CHECK_CXX_SYMBOL_EXISTS("shout_set_content_format" ${LIBSHOUT_HEADER}
	LIBSHOUT_HAS_CONTENT_FORMAT)

if(HAVE_SHOUT_TLS_AUTO AND HAVE_SHOUT_TLS_AUTO_NO_PLAIN AND
		HAVE_SHOUT_TLS_RFC2818 AND HAVE_SHOUT_TLS_RFC2817 AND
		HAVE_SHOUT_TLS_DISABLED AND HAVE_SHOUT_SET_TLS)
	set(LIBSHOUT_HAS_TLS TRUE)
else()
	set(LIBSHOUT_HAS_TLS FALSE)
endif()

# check for shout_set_metadata_utf8() - introduced in libshout v2.4.6
CHECK_CXX_SYMBOL_EXISTS("shout_set_metadata_utf8" ${LIBSHOUT_HEADER}
	HAVE_SHOUT_SET_METADATA_UTF8)
if(HAVE_SHOUT_SET_METADATA_UTF8)
	set(SHOUT_SET_METADATA "shout_set_metadata_utf8")
else()
	set(SHOUT_SET_METADATA "shout_set_metadata")
endif()

set(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES_SAVE})
set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES_SAVE})
set(CMAKE_REQUIRED_LINK_OPTIONS ${CMAKE_REQUIRED_LINK_OPTIONS_SAVE})

option(NFM "Enable support for narrow FM channels" OFF)

set(PLATFORM "native" CACHE STRING "Optimize the build for the given hardware platform")

option(RTLSDR "Enable RTL-SDR support" ON)
set(WITH_RTLSDR FALSE)

option(MIRISDR "Enable Mirics support" ON)
set(WITH_MIRISDR FALSE)

option(SOAPYSDR "Enable SoapySDR support" ON)
set(WITH_SOAPYSDR FALSE)

option(PULSEAUDIO "Enable PulseAudio support" ON)
set(WITH_PULSEAUDIO FALSE)

option(PROFILING "Enable profiling with gperftools")
set(WITH_PROFILING FALSE)

if(RTLSDR)
	find_package(RTLSDR)
	if(RTLSDR_FOUND)
		list(APPEND rtl_airband_extra_sources input-rtlsdr.cpp)
		list(APPEND rtl_airband_extra_libs ${RTLSDR_LIBRARIES})
		list(APPEND rtl_airband_include_dirs ${RTLSDR_INCLUDE_DIRS})
		list(APPEND link_dirs ${RTLSDR_LIBRARY_DIRS})
		set(WITH_RTLSDR TRUE)
	endif()
endif()

if(MIRISDR)
	find_package(MiriSDR)
	if(MIRISDR_FOUND)
		set(WITH_MIRISDR TRUE)
		list(APPEND rtl_airband_extra_sources input-mirisdr.cpp)
		list(APPEND rtl_airband_extra_libs ${MIRISDR_LIBRARIES})
		list(APPEND rtl_airband_include_dirs ${MIRISDR_INCLUDE_DIRS})
		list(APPEND link_dirs ${MIRISDR_LIBRARY_DIRS})
	endif()
endif()

if(SOAPYSDR)
	message(STATUS "Checking for SoapySDR")
	find_package(SoapySDR NO_MODULE)
	if(SoapySDR_FOUND)
		list(APPEND rtl_airband_extra_sources input-soapysdr.cpp)
		message(STATUS "  SoapySDR found, ${SoapySDR_INCLUDE_DIRS}, ${SoapySDR_LIBRARIES}")
		list(APPEND rtl_airband_extra_libs ${SoapySDR_LIBRARIES})
		list(APPEND rtl_airband_include_dirs ${SoapySDR_INCLUDE_DIRS})
		set(WITH_SOAPYSDR TRUE)
	else()
		message(STATUS "  SoapySDR not found")
	endif()
endif()

if(PULSEAUDIO)
	pkg_check_modules(PULSEAUDIO libpulse)
	if(PULSEAUDIO_FOUND)
		list(APPEND rtl_airband_extra_sources pulse.cpp)
		list(APPEND rtl_airband_extra_libs ${PULSEAUDIO_LIBRARIES})
		list(APPEND rtl_airband_include_dirs ${PULSEAUDIO_INCLUDE_DIRS})
		list(APPEND link_dirs ${PULSEAUDIO_LIBRARY_DIRS})
		set(WITH_PULSEAUDIO TRUE)
	endif()
endif()

if(PROFILING)
	pkg_check_modules(PROFILING libprofiler)
	if(PROFILING_FOUND)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
		list(APPEND rtl_airband_extra_libs ${PROFILING_LIBRARIES})
		list(APPEND rtl_airband_include_dirs ${PROFILING_INCLUDE_DIRS})
		list(APPEND link_dirs ${PROFILING_LIBRARY_DIRS})
		set(WITH_PROFILING TRUE)
	endif()
endif()


option(BCM_VC "Enable Broadcom Videocore 3 support" OFF)
set(WITH_BCM_VC FALSE)

# error out on depricated PLATFORM values
if(PLATFORM STREQUAL "rpiv1" OR PLATFORM STREQUAL "armv7-generic" OR PLATFORM STREQUAL "armv8-generic")
	message(FATAL_ERROR "platform '${PLATFORM}' has been deprecated, see https://github.com/rtl-airband/RTLSDR-Airband/discussions/447")
# rpiv2 - Raspberry Pi 2 or Raspberry Pi 3 using Broadcom VideoCore IV GPU for FFT
# NOTE: use 'native' to not use the GPU for FFT
elseif(PLATFORM STREQUAL "rpiv2")
	set(BCM_VC ON)
	add_compile_options(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard)
	enable_language(ASM)
	list(APPEND rtl_airband_extra_sources rtl_airband_neon.s)
# native - let the complier optimize to run on local hardware (default)
elseif(PLATFORM STREQUAL "native")
	CHECK_CXX_COMPILER_FLAG(-march=native CXX_HAS_MARCH_NATIVE)
	if(CXX_HAS_MARCH_NATIVE)
		add_compile_options(-march=native)
	else()
		message(FATAL_ERROR "Cannot build with PLATFORM=native: the compiler does not support -march=native option")
	endif()
# generic - dont add any hardware related flags, used to build a "portable" binary
elseif(PLATFORM STREQUAL "generic")
	# NO-OP
# error out on unrecongnnized PLATFORM value
else()
	message(FATAL_ERROR "Unknown platform '${PLATFORM}'. Valid options are: rpiv2, native, and generic")
endif()

# Try using VC GPU if enabled. Fallback to fftw3f if disabled or if VC lib not found
if(BCM_VC)
	find_package(BCM_VC)
	if(BCM_VC_FOUND)
		add_subdirectory(hello_fft)
		list(APPEND rtl_airband_obj_files $<TARGET_OBJECTS:hello_fft>)
		list(APPEND rtl_airband_extra_libs ${BCM_VC_LIBRARIES})
		set(WITH_BCM_VC TRUE)
	endif()
endif()
if(NOT BCM_VC_FOUND)
	pkg_check_modules(FFTW3F REQUIRED fftw3f)
	if(FFTW3F_FOUND)
		list(APPEND rtl_airband_extra_libs ${FFTW3F_LIBRARIES})
		list(APPEND rtl_airband_include_dirs ${FFTW3F_INCLUDE_DIRS})
		list(APPEND link_dirs ${FFTW3F_LIBRARY_DIRS})
	endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
	list(APPEND rtl_airband_extra_libs c++)
endif()

if(BUILD_UNITTESTS)
	set(BUILD_UNITTESTS TRUE)
else()
	set(BUILD_UNITTESTS FALSE)
endif()

message(STATUS "RTLSDR-Airband configuration summary:\n")
message(STATUS "- Version string:\t\t${RTL_AIRBAND_VERSION}")
message(STATUS "- Build type:\t\t${CMAKE_BUILD_TYPE}")
message(STATUS "- Operating system:\t\t${CMAKE_SYSTEM_NAME}")
message(STATUS "- SDR drivers:")
message(STATUS "  - librtlsdr:\t\trequested: ${RTLSDR}, enabled: ${WITH_RTLSDR}")
message(STATUS "  - mirisdr:\t\t\trequested: ${MIRISDR}, enabled: ${WITH_MIRISDR}")
message(STATUS "  - soapysdr:\t\trequested: ${SOAPYSDR}, enabled: ${WITH_SOAPYSDR}")
message(STATUS "- Other options:")
message(STATUS "  - Platform:\t\t${PLATFORM}")
message(STATUS "  - Build Unit Tests:\t${BUILD_UNITTESTS}")
message(STATUS "  - Broadcom VideoCore GPU:\t${WITH_BCM_VC}")
message(STATUS "  - NFM support:\t\t${NFM}")
message(STATUS "  - PulseAudio:\t\trequested: ${PULSEAUDIO}, enabled: ${WITH_PULSEAUDIO}")
message(STATUS "  - Profiling:\t\trequested: ${PROFILING}, enabled: ${WITH_PROFILING}")
message(STATUS "  - Icecast TLS support:\t${LIBSHOUT_HAS_TLS}")

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config.h)
	message(FATAL_ERROR "${CMAKE_CURRENT_SOURCE_DIR}/config.h nolonger used, delete before continuing")
endif()

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/config.h"
	@ONLY
)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/version.cpp
		${CMAKE_CURRENT_BINARY_DIR}/_version.cpp
	COMMAND ${CMAKE_COMMAND} -DRTL_AIRBAND_VERSION=${RTL_AIRBAND_VERSION} -P
	${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules/version.cmake
)

add_library (rtl_airband_base OBJECT
	config.cpp
	input-common.cpp
	input-file.cpp
	input-helpers.cpp
	mixer.cpp
	output.cpp
	rtl_airband.cpp
	squelch.cpp
	ctcss.cpp
	util.cpp
	udp_stream.cpp
	logging.cpp
	filters.cpp
	helper_functions.cpp
	${CMAKE_CURRENT_BINARY_DIR}/version.cpp
	${rtl_airband_extra_sources}
	)

target_include_directories (rtl_airband_base PUBLIC
	${CMAKE_CURRENT_BINARY_DIR} # needed for config.h
	${rtl_airband_include_dirs}
)

# can't do this per target with cmake <3.13
link_directories(${link_dirs})

list(APPEND rtl_airband_obj_files $<TARGET_OBJECTS:rtl_airband_base>)

add_executable (rtl_airband ${rtl_airband_obj_files})
set_property(TARGET rtl_airband PROPERTY ENABLE_EXPORTS 1)

# add include for config.h
target_include_directories (rtl_airband PUBLIC
	${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries (rtl_airband
	dl
	m
	pthread
	${rtl_airband_extra_libs}
)

install(TARGETS rtl_airband
	RUNTIME DESTINATION bin
)

# TODO: install config if not present


if(BUILD_UNITTESTS)
	cmake_minimum_required(VERSION 3.1...3.18 FATAL_ERROR)

	# GoogleTest requires at least C++14
	set(CMAKE_CXX_STANDARD 14)

	# set timestamps of URL extracted files to the extraction time
	if(POLICY CMP0135)
		cmake_policy(SET CMP0135 NEW)
	endif()

	# pull in GoogleTest as a dependency
	include(FetchContent)
	FetchContent_Declare(
		googletest
		URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
	)
	FetchContent_MakeAvailable(googletest)

	enable_testing()

	file(GLOB_RECURSE TEST_FILES "test_*.cpp")
	list(APPEND TEST_FILES
		squelch.cpp
		logging.cpp
		filters.cpp
		ctcss.cpp
		generate_signal.cpp
		helper_functions.cpp
	)

	add_executable(
		unittests
		${TEST_FILES}
	)
	target_link_libraries(
		unittests
		GTest::gtest_main
		dl
		${rtl_airband_extra_libs}
	)

	# add include for config.h
	target_include_directories (unittests PUBLIC
		${CMAKE_CURRENT_BINARY_DIR}
	)

	include(GoogleTest)
	gtest_discover_tests(unittests)

endif()
