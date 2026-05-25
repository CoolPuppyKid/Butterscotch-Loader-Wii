cmake_minimum_required(VERSION 3.21)
project(butterscotch C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

file(GLOB_RECURSE BUTTERSCOTCH_DATA_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}/data" "${CMAKE_SOURCE_DIR}/data/*")
set(BUTTERSCOTCH_DATA_DEPENDS "")
foreach(data_file IN LISTS BUTTERSCOTCH_DATA_FILES)
    list(APPEND BUTTERSCOTCH_DATA_DEPENDS "${CMAKE_SOURCE_DIR}/data/${data_file}")
endforeach()

set(PLATFORM "" CACHE STRING "Platform backend")
set(AUDIO_BACKEND "" CACHE STRING "Audio backend")

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

# Audio backend defaults
if(AUDIO_BACKEND STREQUAL "")
    if(PLATFORM STREQUAL "web")
        set(AUDIO_BACKEND "web")
    elseif(PLATFORM STREQUAL "web-meta")
        set(AUDIO_BACKEND "none")
    elseif(PLATFORM STREQUAL "ps3")
        set(AUDIO_BACKEND "openal")
    elseif(PLATFORM STREQUAL "ps2")
        set(AUDIO_BACKEND "ps2")
    elseif(PLATFORM STREQUAL "wii")
        set(AUDIO_BACKEND "none")
    else()
        set(AUDIO_BACKEND "miniaudio")
    endif()
endif()

if(AUDIO_BACKEND STREQUAL "openal")
    file(GLOB AUDIO_SOURCES src/audio/openal/*.c)
elseif(AUDIO_BACKEND STREQUAL "miniaudio")
    file(GLOB AUDIO_SOURCES src/audio/miniaudio/*.c)
elseif(AUDIO_BACKEND STREQUAL "web")
    file(GLOB AUDIO_SOURCES src/audio/web/*.c src/audio/miniaudio/*.c)
elseif(AUDIO_BACKEND STREQUAL "ps2")
    file(GLOB AUDIO_SOURCES src/audio/ps2/*.c)
elseif(NOT AUDIO_BACKEND STREQUAL "none")
    message(FATAL_ERROR "Invalid AUDIO_BACKEND!")
endif()

# To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific features
#
# If only a single WAD version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
# performance on low end hardware!
option(ENABLE_WAD14 "Enable support for WAD Version 14 and below" ON)
option(ENABLE_WAD16 "Enable support for WAD Version 15/16" ON)
option(ENABLE_WAD17 "Enable support for WAD Version 17" ON)
option(ENABLE_LEGACY_GL "Enable the legacy OpenGL renderer" ON)
option(ENABLE_MODERN_GL "Enable the modern OpenGL renderer" ON)
if(NOT ENABLE_WAD14 AND NOT ENABLE_WAD16 AND NOT ENABLE_WAD17)
    message(FATAL_ERROR "You need to build Butterscotch with at least one WAD version enabled!")
endif()
if(ENABLE_WAD14)
    add_compile_definitions(ENABLE_WAD14)
endif()
if(ENABLE_WAD16)
    add_compile_definitions(ENABLE_WAD16)
endif()
if(ENABLE_WAD17)
    add_compile_definitions(ENABLE_WAD17)
endif()
if(ENABLE_LEGACY_GL)
    add_compile_definitions(ENABLE_LEGACY_GL)
endif()
if(ENABLE_MODERN_GL)
    add_compile_definitions(ENABLE_MODERN_GL)
endif()

if(PLATFORM STREQUAL "web-meta")
    # Metadata-only build: just the data.win parser and its direct deps. No VM, no runner, no I/O subsystems.
    set(SOURCES
        src/data_win.c
        src/binary_reader.c
    )
else()
    file(GLOB SOURCES src/*.c)
endif()
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

# OpenGL ES 3.0 / WebGL2 compatibility
option(ENABLE_GLES "Build the gl_renderer against OpenGL ES 3.0 / WebGL2 instead of desktop GL 4.1 Core" OFF)
if (ENABLE_GLES AND ENABLE_LEGACY_GL)
    set(ENABLE_LEGACY_GL OFF)
endif()

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(AUDIO_BACKEND STREQUAL "openal")
    target_include_directories(butterscotch PUBLIC src/audio/openal)
    add_compile_definitions(USE_OPENAL)
    if(APPLE)
        set(AUDIO_LIBRARIES "-framework OpenAL")
    elseif(NOT PLATFORM STREQUAL "ps3")
        find_package(OpenAL REQUIRED)
        target_include_directories(butterscotch PUBLIC "${OPENAL_INCLUDE_DIR}")
        set(AUDIO_LIBRARIES "${OPENAL_LIBRARY}")
    endif()
elseif(AUDIO_BACKEND STREQUAL "miniaudio")
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)
    target_include_directories(butterscotch PUBLIC src/audio/miniaudio)
    add_compile_definitions(USE_MINIAUDIO)
elseif(AUDIO_BACKEND STREQUAL "web")
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)
    target_include_directories(butterscotch PUBLIC src/audio/web)
    target_include_directories(butterscotch PUBLIC src/audio/miniaudio)
    add_compile_definitions(USE_WEB_AUDIO)
elseif(AUDIO_BACKEND STREQUAL "ps2")
    target_include_directories(butterscotch PUBLIC src/audio/ps2)
    add_compile_definitions(USE_PS2_AUDIO)
endif()

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

# RFC 1321 reference MD5 implementation
target_sources(butterscotch PRIVATE vendor/md5/md5.c)
target_include_directories(butterscotch PUBLIC vendor/md5)

# Steve Reid's SHA1 implementation
target_sources(butterscotch PRIVATE vendor/sha1/sha1.c)
target_include_directories(butterscotch PUBLIC vendor/sha1)

# Base64 implementation
target_sources(butterscotch PRIVATE vendor/base64/base64.c)
target_include_directories(butterscotch PUBLIC vendor/base64)

# bzip2
add_library(bzip2 STATIC
    vendor/bzip2/blocksort.c
    vendor/bzip2/bzlib.c
    vendor/bzip2/compress.c
    vendor/bzip2/crctable.c
    vendor/bzip2/decompress.c
    vendor/bzip2/huffman.c
    vendor/bzip2/randtable.c
)

target_include_directories(bzip2 PUBLIC vendor/bzip2)

if(PLATFORM STREQUAL "wii")
    file(GLOB IMAGE_SOURCES src/image/*.c)
    target_sources(butterscotch PRIVATE ${IMAGE_SOURCES})

    set(WII_EMBEDDED_DATA_C "${CMAKE_CURRENT_BINARY_DIR}/generated/wii_embedded_data_blob.c")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(
        OUTPUT "${WII_EMBEDDED_DATA_C}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_ROOT=${CMAKE_SOURCE_DIR}/data
            -DOUTPUT_C=${WII_EMBEDDED_DATA_C}
            "-DFILE_LIST=${BUTTERSCOTCH_DATA_FILES}"
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_directory.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed_directory.cmake
        ${BUTTERSCOTCH_DATA_DEPENDS}
        COMMENT "Embedding data/ into Wii binary"
        VERBATIM
    )
    target_sources(butterscotch PRIVATE
        src/wii/wii_embedded_data.c
        "${WII_EMBEDDED_DATA_C}"
    )

    add_compile_definitions(PLATFORM_WII)
    add_compile_definitions(USE_FLOAT_REALS)
    add_compile_definitions(NO_RVALUE_INT64)
    option(BUTTERSCOTCH_WII_ENABLE_AUDIO "Enable the Wii ASND audio backend" OFF)
    if(BUTTERSCOTCH_WII_ENABLE_AUDIO)
        target_compile_definitions(butterscotch PRIVATE BUTTERSCOTCH_WII_ENABLE_AUDIO)
    endif()

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    # stb_vorbis
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    target_compile_options(butterscotch PRIVATE
        -mrvl -mcpu=750 -meabi -mhard-float
        -O2
        -fsingle-precision-constant -Werror=double-promotion
        -fomit-frame-pointer -ffast-math
    )

    target_include_directories(butterscotch PRIVATE
        $ENV{DEVKITPRO}/libogc/include
        $ENV{DEVKITPRO}/portlibs/ppc/include
        $ENV{DEVKITPRO}/portlibs/wii/include
        ${CMAKE_SOURCE_DIR}/src/image
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_directories(butterscotch PRIVATE
        $ENV{DEVKITPRO}/libogc/lib/wii
        $ENV{DEVKITPRO}/portlibs/ppc/lib
        $ENV{DEVKITPRO}/portlibs/wii/lib
        $ENV{DEVKITPRO}/portlibs/wii/lib/lib
    )
    target_link_libraries(butterscotch wiikeyboard wiiuse bte fat asnd ogc m bz2)
    target_compile_definitions(butterscotch PRIVATE IS_BIG_ENDIAN=1)
    target_compile_definitions(butterscotch PRIVATE GEKKO)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-variable -Wno-unused-parameter -Wno-unused-function")

    # Generate .dol from ELF
    add_custom_command(TARGET butterscotch POST_BUILD
        COMMAND $ENV{DEVKITPRO}/tools/bin/elf2dol
            $<TARGET_FILE:butterscotch>
            $<TARGET_FILE_DIR:butterscotch>/butterscotch.dol
        COMMENT "Converting ELF to DOL"
    )
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
