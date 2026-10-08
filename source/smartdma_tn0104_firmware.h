/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * smartdma_tn0104_firmware.h
 *
 * Interface to the prebuilt SmartDMA EZH firmware image for the Kyocera
 * TN0104ANVAANN-GN00 176x176 monochrome MIP LCD panel.
 */

#ifndef SMARTDMA_TN0104_FIRMWARE_H_
#define SMARTDMA_TN0104_FIRMWARE_H_

#include <stdint.h>

/* Base address of the SmartDMA private RAM (RAMX) the firmware runs from. */
#define TN0104_SMARTDMA_MEM_ADDR 0x04000000U

#if defined(__cplusplus)
extern "C" {
#endif

/* Prebuilt SmartDMA firmware image (position-independent EZH machine code). */
extern const uint8_t g_tn0104SmartdmaFirmware[];

/* Size of g_tn0104SmartdmaFirmware in bytes. */
extern const uint32_t g_tn0104SmartdmaFirmwareSize;

#if defined(__cplusplus)
}
#endif

#endif /* SMARTDMA_TN0104_FIRMWARE_H_ */