# Copyright 2026 NXP
# SPDX-License-Identifier: BSD-3-Clause
#
# VS Code for MCUXpresso standalone (cmake-freestanding) build for
# an-mcxa174-mip-panel-demo.
#
# This project follows the official Application Code Hub self-contained layout:
# the required MCUXpresso SDK sources (CMSIS, device, startup, drivers,
# components) are embedded in-tree under arch/ devices/ drivers/ components/,
# and are listed explicitly below. No external mcuxsdk repository is required.
#
# Target : NXP MCXA174VLL (Cortex-M33F, single-precision hard FPU)
# Library: Redlib source path (__REDLIB__ + __MCUXPRESSO), linked with
#          newlib-nano / nosys via the project-local Code-Red linker that
#          provides SRAMX0 + .SMARTDMA_CODE for the SmartDMA TN0104 firmware.

# CROSS COMPILER SETTING
set(CMAKE_SYSTEM_NAME Generic)
cmake_minimum_required(VERSION 3.10.0)

if (NOT DEFINED SdkRootDirPath)
    SET(SdkRootDirPath ${CMAKE_CURRENT_LIST_DIR})
endif()

include(${CMAKE_CURRENT_LIST_DIR}/config.cmake)
include(${SdkRootDirPath}/cmake/toolchain/toolchain.cmake)

# THE VERSION NUMBER
SET (MCUXPRESSO_CMAKE_FORMAT_MAJOR_VERSION 2)
SET (MCUXPRESSO_CMAKE_FORMAT_MINOR_VERSION 0)

include(ide_overrides.cmake OPTIONAL)

if(CMAKE_SCRIPT_MODE_FILE)
  message("${MCUXPRESSO_CMAKE_FORMAT_MAJOR_VERSION}")
  return()
endif()


set(CMAKE_EXECUTABLE_LIBRARY_PREFIX)
set(CMAKE_EXECUTABLE_LIBRARY_SUFFIX)

# CURRENT DIRECTORY
set(ProjDirPath ${CMAKE_CURRENT_SOURCE_DIR})

set(EXECUTABLE_OUTPUT_PATH ${ProjDirPath}/${CMAKE_BUILD_TYPE})
set(LIBRARY_OUTPUT_PATH ${ProjDirPath}/${CMAKE_BUILD_TYPE})


project(an-mcxa174-mip-panel-demo)

enable_language(ASM)

set(MCUX_BUILD_TYPES debug)

# Keep all build artifacts (elf/bin/hex/map) inside the build directory
# (e.g. debug/) instead of scattering them at the project root.
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(MCUX_SDK_PROJECT_NAME ${PROJECT_NAME}.elf)

include(${ProjDirPath}/flags.cmake)

add_executable(${MCUX_SDK_PROJECT_NAME}
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/system_MCXA174.c"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/startup_MCXA174.c"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/drivers/fsl_clock.c"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/drivers/fsl_reset.c"
"${ProjDirPath}/board/board.c"
"${ProjDirPath}/board/clock_config.c"
"${ProjDirPath}/board/pin_mux.c"
"${ProjDirPath}/board/hardware_init.c"
"${ProjDirPath}/drivers/common/fsl_common.c"
"${ProjDirPath}/drivers/common/fsl_common_arm.c"
"${ProjDirPath}/drivers/gpio/fsl_gpio.c"
"${ProjDirPath}/drivers/lpuart/fsl_lpuart.c"
"${ProjDirPath}/drivers/mcx_cmc/fsl_cmc.c"
"${ProjDirPath}/drivers/mcx_spc/fsl_spc.c"
"${ProjDirPath}/drivers/waketimer/fsl_waketimer.c"
"${ProjDirPath}/drivers/wuu/fsl_wuu.c"
"${ProjDirPath}/components/assert/fsl_assert.c"
"${ProjDirPath}/components/debug_console_lite/fsl_debug_console.c"
"${ProjDirPath}/components/misc_utilities/fsl_syscall_stub.c"
"${ProjDirPath}/components/misc_utilities/fsl_sbrk.c"
"${ProjDirPath}/components/misc_utilities/fsl_memcpy.S"
"${ProjDirPath}/components/str/fsl_str.c"
"${ProjDirPath}/components/uart/fsl_adapter_lpuart.c"
"${ProjDirPath}/source/main.c"
"${ProjDirPath}/source/smartdma_tn0104_firmware.c"

"${ProjDirPath}/source/mcux_config.h"
"${ProjDirPath}/source/mcuxsdk_version.h"
"${ProjDirPath}/source/smartdma_tn0104_firmware.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/fsl_device_registers.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/MCXA174.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/MCXA174_features.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/MCXA174_COMMON.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/system_MCXA174.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/drivers/fsl_clock.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/drivers/fsl_inputmux_connections.h"
"${ProjDirPath}/devices/MCX/MCXA/MCXA174/drivers/fsl_reset.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/cmsis_compiler.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/cmsis_version.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/cmsis_gcc.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/m-profile/cmsis_gcc_m.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/core_cm33.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/m-profile/armv8m_mpu.h"
"${ProjDirPath}/arch/arm/CMSIS/Core/Include/tz_context.h"
"${ProjDirPath}/board/board.h"
"${ProjDirPath}/board/clock_config.h"
"${ProjDirPath}/board/pin_mux.h"
"${ProjDirPath}/board/app.h"
"${ProjDirPath}/drivers/common/fsl_common.h"
"${ProjDirPath}/drivers/common/fsl_common_arm.h"
"${ProjDirPath}/drivers/gpio/fsl_gpio.h"
"${ProjDirPath}/drivers/lpuart/fsl_lpuart.h"
"${ProjDirPath}/drivers/mcx_cmc/fsl_cmc.h"
"${ProjDirPath}/drivers/mcx_spc/fsl_spc.h"
"${ProjDirPath}/drivers/waketimer/fsl_waketimer.h"
"${ProjDirPath}/drivers/wuu/fsl_wuu.h"
"${ProjDirPath}/drivers/port/fsl_port.h"
"${ProjDirPath}/components/assert/fsl_assert.h"
"${ProjDirPath}/components/debug_console_lite/fsl_debug_console.h"
"${ProjDirPath}/components/str/fsl_str.h"
"${ProjDirPath}/components/uart/fsl_adapter_uart.h"
)

target_include_directories(${MCUX_SDK_PROJECT_NAME} PRIVATE
    ${ProjDirPath}/devices/MCX/MCXA/MCXA174
    ${ProjDirPath}/devices/MCX/MCXA/periph3
    ${ProjDirPath}/devices/MCX/MCXA/MCXA174/drivers
    ${ProjDirPath}/arch/arm/CMSIS/Core/Include
    ${ProjDirPath}/arch/arm/CMSIS/Core/Include/m-profile
    ${ProjDirPath}/board
    ${ProjDirPath}/source
    ${ProjDirPath}/drivers/common
    ${ProjDirPath}/drivers/gpio
    ${ProjDirPath}/drivers/lpuart
    ${ProjDirPath}/drivers/mcx_cmc
    ${ProjDirPath}/drivers/mcx_spc
    ${ProjDirPath}/drivers/port
    ${ProjDirPath}/drivers/waketimer
    ${ProjDirPath}/drivers/wuu
    ${ProjDirPath}/components/assert
    ${ProjDirPath}/components/debug_console_lite
    ${ProjDirPath}/components/str
    ${ProjDirPath}/components/uart
)


IF(NOT DEFINED TARGET_LINK_SYSTEM_LIBRARIES)
    SET(TARGET_LINK_SYSTEM_LIBRARIES "-lm -lc -lgcc -lnosys")
ENDIF()

TARGET_LINK_LIBRARIES(${MCUX_SDK_PROJECT_NAME} PRIVATE -Wl,--start-group)

target_link_libraries(${MCUX_SDK_PROJECT_NAME} PRIVATE ${TARGET_LINK_SYSTEM_LIBRARIES})

TARGET_LINK_LIBRARIES(${MCUX_SDK_PROJECT_NAME} PRIVATE -Wl,--end-group)

ADD_CUSTOM_COMMAND(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD COMMAND ${CMAKE_OBJCOPY}
-Obinary ${EXECUTABLE_OUTPUT_PATH}/${MCUX_SDK_PROJECT_NAME} ${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}.bin)

ADD_CUSTOM_COMMAND(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD COMMAND ${CMAKE_OBJCOPY}
-Oihex ${EXECUTABLE_OUTPUT_PATH}/${MCUX_SDK_PROJECT_NAME} ${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}.hex)

set_target_properties(${MCUX_SDK_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES "output.map;${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}.bin;${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}.hex")
