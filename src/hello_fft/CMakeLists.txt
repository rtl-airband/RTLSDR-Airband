set(hello_fft_source_files
	mailbox.c
	gpu_fft.c
	gpu_fft_twiddles.c
	gpu_fft_shaders.c
	gpu_fft_base.c
)
# Temp hack due to the fact that mailbox.c includes ../rtl_airband.h which
# is a C++ header.
SET_SOURCE_FILES_PROPERTIES(${hello_fft_source_files} PROPERTIES LANGUAGE CXX )
add_library(hello_fft OBJECT
	${hello_fft_source_files}
)
target_include_directories(hello_fft PUBLIC
	".." # needed for rtl_airband.h
	"${CMAKE_CURRENT_BINARY_DIR}/.." # needed for config.h
)

# disable -Wcast-qual for this folder
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-cast-qual")
