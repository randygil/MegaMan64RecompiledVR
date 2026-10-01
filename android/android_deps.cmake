# Dependencies for the Android build, which is cross compiled from a desktop host.
# Included by the top level CMakeLists.txt when ANDROID is set.

include(FetchContent)

# Host tools. file_to_c runs during the build, so it has to be a host executable. Build the desktop version
# of the project first (or just its file_to_c target) and point RECOMP_HOST_FILE_TO_C at it.
if (NOT DEFINED RECOMP_HOST_FILE_TO_C)
    message(FATAL_ERROR "Set RECOMP_HOST_FILE_TO_C to a file_to_c executable built for the host.")
endif()
add_executable(file_to_c IMPORTED GLOBAL)
set_target_properties(file_to_c PROPERTIES IMPORTED_LOCATION "${RECOMP_HOST_FILE_TO_C}")

# SDL2, built as libSDL2.so and used by the Java SDLActivity.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl2_android
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-2.30.12.tar.gz
)
FetchContent_MakeAvailable(sdl2_android)

# The game includes SDL as "SDL2/SDL.h" outside of Windows, so expose the headers under an SDL2 folder too.
set(SDL2_PREFIXED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/sdl2_prefixed_include")
file(MAKE_DIRECTORY "${SDL2_PREFIXED_INCLUDE_DIR}/SDL2")
file(GLOB SDL2_PUBLIC_HEADERS "${sdl2_android_SOURCE_DIR}/include/*.h")
file(COPY ${SDL2_PUBLIC_HEADERS} DESTINATION "${SDL2_PREFIXED_INCLUDE_DIR}/SDL2")
include_directories("${SDL2_PREFIXED_INCLUDE_DIR}" "${sdl2_android_SOURCE_DIR}/include")

# zstd's dictionary builder needs qsort_r, which bionic lacks, and RT64 only decompresses.
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)

# FreeType for RmlUi.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype_android
    URL https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz
)
FetchContent_MakeAvailable(freetype_android)
if (NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
set(FREETYPE_INCLUDE_DIRS "${freetype_android_SOURCE_DIR}/include")
set(FREETYPE_LIBRARIES freetype)

# find_package(Freetype) in RmlUi resolves to the target above.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
