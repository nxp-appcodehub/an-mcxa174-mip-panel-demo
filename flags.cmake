# Copyright 2026 NXP
# SPDX-License-Identifier: BSD-3-Clause
#
# Compile / assemble / link flag recipe for an-mcxa174-mip-panel-demo.
#
# This is the standalone (cmake-freestanding) ACH form. The flags below
# reproduce the exact arm-none-eabi-gcc recipe captured from the reference
# MCUXpresso build:
#   - Cortex-M33, single-precision hard FPU (fpv5-sp-d16).
#   - Redlib source path (__REDLIB__ + __MCUXPRESSO) so the SDK startup runs
#     the multi-section __data_section_table copy loop that relocates
#     .SMARTDMA_CODE into SRAMX0. Link is done with newlib-nano / nosys
#     (-lm -lc -lgcc -lnosys) plus the SDK syscall/sbrk stubs.
#   - The final image is linked with the project-local Code-Red master linker
#     linker/an-mcxa174-mip-panel-demo_Debug.ld, which INCLUDEs the *_library.ld
#     and *_memory.ld sub-scripts. ld resolves INCLUDE via -L search paths, so
#     linker/ is added with -L.

IF(NOT DEFINED FPU)
    SET(FPU "-mfloat-abi=hard -mfpu=fpv5-sp-d16")
ENDIF()

IF(NOT DEFINED SPECS)
    SET(SPECS "--specs=nosys.specs")
ENDIF()

IF(NOT DEFINED DEBUG_CONSOLE_CONFIG)
    SET(DEBUG_CONSOLE_CONFIG "-DSDK_DEBUGCONSOLE=1")
ENDIF()

SET(CMAKE_ASM_FLAGS_DEBUG " \
    ${CMAKE_ASM_FLAGS_DEBUG} \
    -DCPU_MCXA174VLL \
    -DMCXA174_SERIES \
    -D__STARTUP_CLEAR_BSS \
    -DMCUXPRESSO_SDK \
    -D__MCUXPRESSO \
    -D__REDLIB__ \
    -include \
    ${ProjDirPath}/source/mcux_config.h \
    -include \
    ${ProjDirPath}/source/mcuxsdk_version.h \
    -mthumb \
    -mcpu=cortex-m33 \
    -g \
    ${FPU} \
")
SET(CMAKE_C_FLAGS_DEBUG " \
    ${CMAKE_C_FLAGS_DEBUG} \
    -DCPU_MCXA174VLL \
    -DMCXA174_SERIES \
    -D__STARTUP_CLEAR_BSS \
    -DDEBUG \
    -DMCUX_META_BUILD \
    -DMCUXPRESSO_SDK \
    -D__MCUXPRESSO \
    -D__REDLIB__ \
    -D__USE_CMSIS \
    -DCR_INTEGER_PRINTF \
    -DPRINTF_FLOAT_ENABLE=0 \
    -DSDK_OS_BAREMETAL \
    -include \
    ${ProjDirPath}/source/mcux_config.h \
    -include \
    ${ProjDirPath}/source/mcuxsdk_version.h \
    --specs=nano.specs \
    -Wall \
    -fno-common \
    -ffunction-sections \
    -fdata-sections \
    -fno-builtin \
    -mthumb \
    -mapcs \
    -std=gnu99 \
    -Werror \
    -fstack-usage \
    -mcpu=cortex-m33 \
    -g \
    -O0 \
    ${FPU} \
    ${DEBUG_CONSOLE_CONFIG} \
")
SET(CMAKE_EXE_LINKER_FLAGS_DEBUG " \
    ${CMAKE_EXE_LINKER_FLAGS_DEBUG} \
    -Wall \
    -fno-common \
    -ffunction-sections \
    -fdata-sections \
    -fno-builtin \
    -mthumb \
    -mapcs \
    -Wl,--gc-sections \
    -Wl,-static \
    -Wl,--print-memory-usage \
    -Xlinker \
    -Map=output.map \
    -mcpu=cortex-m33 \
    -g \
    -Wl,--no-warn-rwx-segments \
    ${FPU} \
    ${SPECS} \
-L\"${ProjDirPath}/linker\" \
-T\"${ProjDirPath}/linker/an-mcxa174-mip-panel-demo_Debug.ld\" -static \
")
