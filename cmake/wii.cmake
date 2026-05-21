set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

if(DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO $ENV{DEVKITPRO})
else()
    message(FATAL_ERROR "DEVKITPRO environment variable is not set!")
endif()

if(DEFINED ENV{DEVKITPPC})
    set(DEVKITPPC $ENV{DEVKITPPC})
else()
    message(FATAL_ERROR "DEVKITPPC environment variable is not set!")
endif()

set(WII_FLAGS "-mrvl -mcpu=750 -meabi -mhard-float -ffunction-sections -fdata-sections -I${DEVKITPRO}/libogc/include -I${DEVKITPRO}/portlibs/wii/include")

set(CMAKE_C_COMPILER ${DEVKITPPC}/bin/powerpc-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITPPC}/bin/powerpc-eabi-g++)
set(CMAKE_AR ${DEVKITPPC}/bin/powerpc-eabi-ar)

set(CMAKE_C_FLAGS_INIT ${WII_FLAGS})
set(CMAKE_CXX_FLAGS_INIT ${WII_FLAGS})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mrvl -mcpu=750 -meabi -mhard-float -Wl,--gc-sections -L${DEVKITPRO}/libogc/lib/wii -L${DEVKITPRO}/portlibs/wii/lib")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(CMAKE_FIND_ROOT_PATH ${DEVKITPPC} ${DEVKITPRO}/portlibs/wii ${DEVKITPRO}/libogc)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(WII TRUE)
set(PLATFORM_WII TRUE)