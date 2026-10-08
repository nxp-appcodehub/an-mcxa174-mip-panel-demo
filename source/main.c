/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * main.c
 *
 * MIP (Memory In Pixel) Panel driving Demo for FRDM-MCXA174
 *
 * Demonstrates driving a Kyocera TN0104ANVAANN-GN00 176x176 monochrome
 * Memory-In-Pixel (MIP) LCD panel using the MCX A series SmartDMA peripheral.
 *
 * Features:
 *   - 176x176 pixel monochrome MIP LCD display
 *   - Real-time clock: time (HH:MM:SS), date (YYYY-MM-DD), and weekday
 *   - 7-segment style time display with small/medium digit sizes
 *   - Partial screen refresh: only changed regions are redrawn each second
 *   - Low-power operation: selectable Deep Sleep or Power Down mode between
 *     1-second update cycles; Power Down is the default for lowest average
 *     current consumption
 *   - NXP logo displayed at the bottom of the watch face
 *
 * Architecture:
 *   - CPU (Cortex-M33): renders UI content into a 1-bit frame buffer in SRAM
 *   - SmartDMA: autonomously generates all MIP panel GPIO timing waveforms
 *   - WAKETIMER: provides a 1 Hz periodic wake-up for timekeeping
 *   - WUU (Wake-Up Unit): configured as WAKETIMER interrupt wake-up source
 *     for Power Down mode
 */

#include "fsl_device_registers.h"
#include "board.h"
#include "smartdma_tn0104_firmware.h"
#include "app.h"
#include "fsl_waketimer.h"
#include "fsl_cmc.h"
#include "fsl_spc.h"
#include "fsl_wuu.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/******************************************************************************
 * Display Configuration
 *****************************************************************************/
#define SCREEN_WIDTH              176U                             /* Panel width in pixels            */
#define SCREEN_HEIGHT             176U                             /* Panel height in pixels           */
#define BYTES_PER_ROW              22U                             /* 176 pixels / 8 bits = 22 bytes   */
#define FRAME_BUFFER_SIZE         (SCREEN_HEIGHT * BYTES_PER_ROW)  /* Total frame buffer size (bytes)  */

/* Frame buffer (1-bit per pixel, monochrome) */
static volatile uint8_t g_frameBuffer[FRAME_BUFFER_SIZE];

/* Dedicated buffer for seconds area (optimized partial refresh) */
#define SEC_REGION_WIDTH           32U  /* 32 pixels: two small digits plus spacing */
#define SEC_REGION_HEIGHT          28U  /* 28 rows: matches small digit height      */
#define SEC_REGION_BYTES_PER_ROW    4U  /* 32 pixels / 8 bits = 4 bytes per row     */
#define SEC_BUFFER_SIZE            (SEC_REGION_HEIGHT * SEC_REGION_BYTES_PER_ROW)

static volatile uint8_t g_secondsBuffer[SEC_BUFFER_SIZE];

/******************************************************************************
 * SMARTDMA LCD Parameter Structure
 *
 * Passed to the SmartDMA EZH firmware via the ARM2EZH register to describe
 * the frame buffer region to transfer for each display update.
 *****************************************************************************/
typedef struct _smartdma_lcd_param
{
    uint32_t *p_data;  /* Pointer to the start of the data buffer             */
    uint32_t  v_addr;  /* Vertical (row) start address, range 0..175          */
    uint32_t  h_addr;  /* Horizontal start address in bytes (8-pixel aligned) */
    uint32_t  h_len;   /* Horizontal transfer length in bytes per row         */
    uint32_t  v_len;   /* Number of rows to transfer                          */
} smartdma_lcd_param_t;

static smartdma_lcd_param_t g_lcdParam;

/******************************************************************************
 * Watch Time and Date State
 *****************************************************************************/
static uint8_t  g_watchHour    = 0;
static uint8_t  g_watchMinute  = 0;
static uint8_t  g_watchSecond  = 0;
static uint16_t g_watchYear    = 2026;
static uint8_t  g_watchMonth   = 1;
static uint8_t  g_watchDay     = 1;
static uint8_t  g_watchWeekday = 4;  /* 0=Sunday ... 6=Saturday; initialized to Thursday */

static const char *const g_weekdayStr[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

/* Flag set by WAKETIMER ISR each second to request a display refresh */
static volatile uint8_t g_needRefresh = 0;

/******************************************************************************
 * Display Layout Position Constants
 * All coordinates are in logical pixels (before display-orientation flip).
 *****************************************************************************/
#define TIME_START_X   15   /* X origin for the hour tens digit    */
#define TIME_Y         62   /* Y origin for all hour/minute digits */
#define SEC_X          136  /* X origin for the seconds tens digit */
#define SEC_Y          70   /* Y origin for the seconds digits     */

/* WAKETIMER reload value for a calibrated 1-second interval */
#define WAKETIMER_1S_COUNT  1031U

/******************************************************************************
 * Power Mode Selection
 ******************************************************************************/

/**
 * @brief Supported low-power modes for the display update idle period.
 */
typedef enum _app_power_mode
{
    kAPP_PowerModeDeepSleep, /*!< Deep Sleep mode */
    kAPP_PowerModePowerDown, /*!< Power Down mode */
} app_power_mode_t;

/* Active low-power mode. Default: Power Down for lowest average current. */
static app_power_mode_t g_targetPowerMode = kAPP_PowerModePowerDown;

/******************************************************************************
 * Basic Graphics Primitives
 *****************************************************************************/

/**
 * @brief Set or clear a single pixel in a frame buffer.
 *
 * Applies a 180-degree coordinate rotation so that (0, 0) maps to the
 * top-left corner of the visible display.
 *
 * @param framebuffer  Pointer to the target frame buffer.
 * @param x            Logical X coordinate (0 = left edge).
 * @param y            Logical Y coordinate (0 = top edge).
 * @param color        1 = black pixel, 0 = white pixel.
 */
static void SetPixel(uint8_t *framebuffer, int x, int y, uint8_t color)
{
    if (x >= 0 && x < (int)SCREEN_WIDTH && y >= 0 && y < (int)SCREEN_HEIGHT)
    {
        /* 180-degree rotation to match physical display orientation */
        int fx = (int)SCREEN_WIDTH  - 1 - x;
        int fy = (int)SCREEN_HEIGHT - 1 - y;

        int byte_index = fy * (int)BYTES_PER_ROW + fx / 8;
        int bit_index  = fx % 8;

        if (color)
            framebuffer[byte_index] |=  (uint8_t)(1u << bit_index);
        else
            framebuffer[byte_index] &= ~(uint8_t)(1u << bit_index);
    }
}

/**
 * @brief Set or clear a single pixel in a region (partial-update) buffer.
 *
 * Uses local coordinates where (0, 0) is the top-left of the region.
 * Applies a 180-degree flip relative to the region dimensions.
 *
 * @param framebuffer    Pointer to the region frame buffer.
 * @param bytes_per_row  Bytes per row in the region buffer.
 * @param region_width   Width of the region in pixels.
 * @param region_height  Height of the region in pixels.
 * @param x              Local X coordinate within the region.
 * @param y              Local Y coordinate within the region.
 * @param color          1 = black pixel, 0 = white pixel.
 */
static void SetPixelRegion(uint8_t *framebuffer, int bytes_per_row,
                           int region_width, int region_height,
                           int x, int y, uint8_t color)
{
    if (x >= 0 && x < region_width && y >= 0 && y < region_height)
    {
        int fx = region_width  - 1 - x;
        int fy = region_height - 1 - y;

        int byte_index = fy * bytes_per_row + fx / 8;
        int bit_index  = fx % 8;

        if (color)
            framebuffer[byte_index] |=  (uint8_t)(1u << bit_index);
        else
            framebuffer[byte_index] &= ~(uint8_t)(1u << bit_index);
    }
}

/** @brief Draw a filled rectangle in a frame buffer. */
static void DrawRect(uint8_t *framebuffer, int x, int y,
                     int width, int height, uint8_t color)
{
    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++)
            SetPixel(framebuffer, x + col, y + row, color);
}

/** @brief Draw a filled rectangle in a region (partial-update) buffer. */
static void DrawRectRegion(uint8_t *framebuffer, int bytes_per_row,
                           int region_width, int region_height,
                           int x, int y, int width, int height, uint8_t color)
{
    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++)
            SetPixelRegion(framebuffer, bytes_per_row, region_width, region_height,
                           x + col, y + row, color);
}

/** @brief Clear (fill with white) a rectangular area in a frame buffer. */
static void ClearRect(uint8_t *framebuffer, int x, int y, int width, int height)
{
    DrawRect(framebuffer, x, y, width, height, 0);
}

/******************************************************************************
 * 7-Segment Display Functions
 *
 * Standard 7-segment encoding:
 *   bit 0 = a (top horizontal)        bit 4 = e (left lower vertical)
 *   bit 1 = b (right upper vertical)  bit 5 = f (left upper vertical)
 *   bit 2 = c (right lower vertical)  bit 6 = g (middle horizontal)
 *   bit 3 = d (bottom horizontal)
 *****************************************************************************/

/* Segment patterns for digits 0-9 */
static const uint8_t kSegmentMap[10] = {
    0x3F, /* 0: a b c d e f   */
    0x06, /* 1:   b c         */
    0x5B, /* 2: a b   d e   g */
    0x4F, /* 3: a b c d     g */
    0x66, /* 4:   b c     f g */
    0x6D, /* 5: a   c d   f g */
    0x7D, /* 6: a   c d e f g */
    0x07, /* 7: a b c         */
    0x7F, /* 8: a b c d e f g */
    0x6F, /* 9: a b c d   f g */
};

/**
 * @brief Draw a medium 7-segment digit into a frame buffer (for hours/minutes).
 *
 * Approximate dimensions: 26 x 46 pixels.
 * Segment thickness: 3 px. Segment length: 20 px.
 *
 * @param framebuffer  Pointer to the target frame buffer.
 * @param x            Top-left X coordinate.
 * @param y            Top-left Y coordinate.
 * @param digit        Digit value 0-9.
 */
static void DrawMidDigit(uint8_t *framebuffer, int x, int y, uint8_t digit)
{
    const int t = 3, l = 20;
    uint8_t s = kSegmentMap[digit % 10u];
    if (s & 0x01u) DrawRect(framebuffer, x+t,   y,         l, t, 1); /* a: top         */
    if (s & 0x02u) DrawRect(framebuffer, x+t+l, y+t,       t, l, 1); /* b: upper-right */
    if (s & 0x04u) DrawRect(framebuffer, x+t+l, y+t*2+l,   t, l, 1); /* c: lower-right */
    if (s & 0x08u) DrawRect(framebuffer, x+t,   y+t*2+l*2, l, t, 1); /* d: bottom      */
    if (s & 0x10u) DrawRect(framebuffer, x,     y+t*2+l,   t, l, 1); /* e: lower-left  */
    if (s & 0x20u) DrawRect(framebuffer, x,     y+t,       t, l, 1); /* f: upper-left  */
    if (s & 0x40u) DrawRect(framebuffer, x+t,   y+t+l,     l, t, 1); /* g: middle      */
}

/**
 * @brief Draw a small 7-segment digit into a frame buffer (for seconds).
 *
 * Approximate dimensions: 15 x 27 pixels.
 * Segment thickness: 2 px. Segment length: 11 px.
 *
 * @param framebuffer  Pointer to the target frame buffer.
 * @param x            Top-left X coordinate.
 * @param y            Top-left Y coordinate.
 * @param digit        Digit value 0-9.
 */
static void DrawSmallDigit(uint8_t *framebuffer, int x, int y, uint8_t digit)
{
    const int t = 2, l = 11;
    uint8_t s = kSegmentMap[digit % 10u];
    if (s & 0x01u) DrawRect(framebuffer, x+t,   y,         l, t, 1);
    if (s & 0x02u) DrawRect(framebuffer, x+t+l, y+t,       t, l, 1);
    if (s & 0x04u) DrawRect(framebuffer, x+t+l, y+t*2+l,   t, l, 1);
    if (s & 0x08u) DrawRect(framebuffer, x+t,   y+t*2+l*2, l, t, 1);
    if (s & 0x10u) DrawRect(framebuffer, x,     y+t*2+l,   t, l, 1);
    if (s & 0x20u) DrawRect(framebuffer, x,     y+t,       t, l, 1);
    if (s & 0x40u) DrawRect(framebuffer, x+t,   y+t+l,     l, t, 1);
}

/**
 * @brief Draw a small 7-segment digit into a region (partial-update) buffer.
 *
 * Uses local coordinates within the region buffer.
 *
 * @param framebuffer    Pointer to the region frame buffer.
 * @param bytes_per_row  Bytes per row in the region buffer.
 * @param region_width   Width of the region in pixels.
 * @param region_height  Height of the region in pixels.
 * @param x              Local X coordinate within the region.
 * @param y              Local Y coordinate within the region.
 * @param digit          Digit value 0-9.
 */
static void DrawSmallDigitRegion(uint8_t *framebuffer, int bytes_per_row,
                                 int region_width, int region_height,
                                 int x, int y, uint8_t digit)
{
    const int t = 2, l = 11;
    uint8_t s = kSegmentMap[digit % 10u];
    if (s & 0x01u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x+t,   y,         l, t, 1);
    if (s & 0x02u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x+t+l, y+t,       t, l, 1);
    if (s & 0x04u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x+t+l, y+t*2+l,   t, l, 1);
    if (s & 0x08u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x+t,   y+t*2+l*2, l, t, 1);
    if (s & 0x10u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x,     y+t*2+l,   t, l, 1);
    if (s & 0x20u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x,     y+t,       t, l, 1);
    if (s & 0x40u) DrawRectRegion(framebuffer, bytes_per_row, region_width, region_height, x+t,   y+t+l,     l, t, 1);
}

/** @brief Draw HH:MM colon separator (two 3x3 dot rectangles). */
static void DrawColon(uint8_t *framebuffer, int x, int y)
{
    DrawRect(framebuffer, x, y + 15, 3, 3, 1);
    DrawRect(framebuffer, x, y + 28, 3, 3, 1);
}

/******************************************************************************
 * 5x7 Bitmap Font
 *****************************************************************************/

/**
 * @brief Draw a single character using the 5x7 bitmap font.
 *
 * Supported characters: '0'-'9', 'A'-'Z', '-', ':'.
 * Each character is 5 pixels wide and 7 pixels tall (column-major, LSB=top).
 *
 * @param framebuffer  Pointer to the target frame buffer.
 * @param x            Top-left X coordinate.
 * @param y            Top-left Y coordinate.
 * @param c            Character to draw.
 */
static void DrawSmallChar(uint8_t *framebuffer, int x, int y, char c)
{
    static const uint8_t kFont5x7Digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    };

    static const uint8_t kFont5x7Alpha[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    };

    const uint8_t *font_data = NULL;

    if (c >= '0' && c <= '9')
        font_data = kFont5x7Digits[c - '0'];
    else if (c >= 'A' && c <= 'Z')
        font_data = kFont5x7Alpha[c - 'A'];
    else if (c == '-')
    {
        DrawRect(framebuffer, x, y + 3, 5, 1, 1);
        return;
    }
    else if (c == ':')
    {
        SetPixel(framebuffer, x + 2, y + 2, 1);
        SetPixel(framebuffer, x + 2, y + 5, 1);
        return;
    }

    /* Render column-major bitmap: each byte is one column, LSB = topmost row */
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (font_data[col] & (1u << row))
                SetPixel(framebuffer, x + col, y + row, 1);
}

/**
 * @brief Draw a null-terminated ASCII string using the 5x7 bitmap font.
 *
 * Characters are rendered left-to-right with 1-pixel horizontal spacing.
 *
 * @param framebuffer  Pointer to the target frame buffer.
 * @param x            X coordinate of the first character.
 * @param y            Y coordinate of the string baseline.
 * @param str          Null-terminated string (uppercase ASCII only).
 */
static void DrawSmallString(uint8_t *framebuffer, int x, int y, const char *str)
{
    int offset = 0;
    while (*str != '\0')
    {
        DrawSmallChar(framebuffer, x + offset, y, *str);
        offset += 6;  /* 5-pixel character width + 1-pixel spacing */
        str++;
    }
}

/******************************************************************************
 * NXP Logo Bitmap (80x39 pixels, 1 bpp, MSB-first per byte)
 * 10 bytes per row. Rows 0-9 and 29-38 are blank margins.
 *****************************************************************************/
#define NXP_LOGO_WIDTH          80U
#define NXP_LOGO_HEIGHT         39U
#define NXP_LOGO_BYTES_PER_ROW  (NXP_LOGO_WIDTH / 8U)

static const uint8_t kNxpLogoData[NXP_LOGO_HEIGHT * NXP_LOGO_BYTES_PER_ROW] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x1F,0xC0,0x13,0xF8,0x0F,0xE7,0xFF,0xC0,0x00,
    0x00,0x1F,0xE0,0x13,0xF8,0x1F,0xCF,0xFF,0xE0,0x00,
    0x00,0x1F,0xF0,0x19,0xFC,0x1F,0xDF,0xFF,0xF0,0x00,
    0x00,0x1F,0xF0,0x1D,0xFE,0x3F,0x9F,0xFF,0xF0,0x00,
    0x00,0x1F,0xF8,0x1C,0xFE,0x7F,0x3F,0xFF,0xF8,0x00,
    0x00,0x1F,0xFC,0x1E,0x7F,0x7F,0x38,0x01,0xF8,0x00,
    0x00,0x1F,0xFE,0x1E,0x7F,0xFE,0x78,0x01,0xF8,0x00,
    0x00,0x1F,0xFF,0x1F,0x3F,0xFE,0xF8,0x01,0xF8,0x00,
    0x00,0x1F,0xBF,0x9F,0xBF,0xFC,0xF8,0x01,0xF8,0x00,
    0x00,0x1F,0x9F,0xDF,0x9F,0xF9,0xF8,0x01,0xF8,0x00,
    0x00,0x1F,0x8F,0xDF,0xBF,0xFC,0xFF,0xFF,0xF8,0x00,
    0x00,0x1F,0x87,0xFF,0x3F,0xFE,0xFF,0xFF,0xF0,0x00,
    0x00,0x1F,0x83,0xFE,0x7F,0xFE,0x7F,0xFF,0xF0,0x00,
    0x00,0x1F,0x83,0xFE,0x7F,0x7F,0x3F,0xFF,0xE0,0x00,
    0x00,0x1F,0x81,0xFC,0xFE,0x7F,0x3F,0xFF,0xC0,0x00,
    0x00,0x1F,0x80,0xFD,0xFE,0x3F,0x98,0x00,0x00,0x00,
    0x00,0x1F,0x80,0x79,0xFC,0x1F,0xD8,0x00,0x00,0x00,
    0x00,0x1F,0x80,0x33,0xF8,0x1F,0xC8,0x00,0x00,0x00,
    0x00,0x1F,0x80,0x13,0xF8,0x0F,0xE0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/**
 * @brief Render NXP logo bitmap to the frame buffer at position (x, y).
 * Logo data is MSB-first (bit 7 = leftmost pixel per byte).
 */
static void DrawNXPLogo(uint8_t *framebuffer, int x, int y)
{
    for (int row = 0; row < (int)NXP_LOGO_HEIGHT; row++)
        for (int col = 0; col < (int)NXP_LOGO_WIDTH; col++)
        {
            int bidx = row * (int)NXP_LOGO_BYTES_PER_ROW + col / 8;
            if ((kNxpLogoData[bidx] >> (7 - col % 8)) & 0x01u)
                SetPixel(framebuffer, x + col, y + row, 1);
        }
}

/******************************************************************************
 * Watch Time Update
 *****************************************************************************/

/**
 * @brief Advance the watch time by one second.
 * Handles rollover for seconds, minutes, hours, day, month, and year.
 * Includes leap-year correction for February.
 */
static void Watch_UpdateTime(void)
{
    g_watchSecond++;

    if (g_watchSecond >= 60)
    {
        g_watchSecond = 0;
        g_watchMinute++;

        if (g_watchMinute >= 60)
        {
            g_watchMinute = 0;
            g_watchHour++;

            if (g_watchHour >= 24)
            {
                g_watchHour = 0;
                g_watchDay++;
                g_watchWeekday = (g_watchWeekday + 1) % 7;

                /* Leap year: divisible by 4, except centuries unless div by 400 */
                uint8_t days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                if (g_watchYear % 4u == 0u &&
                    (g_watchYear % 100u != 0u || g_watchYear % 400u == 0u))
                    days_in_month[1] = 29u;

                if (g_watchDay > days_in_month[g_watchMonth - 1])
                {
                    g_watchDay = 1;
                    g_watchMonth++;

                    if (g_watchMonth > 12)
                    {
                        g_watchMonth = 1;
                        g_watchYear++;
                    }
                }
            }
        }
    }
}

/******************************************************************************
 * Screen Rendering Functions
 *****************************************************************************/

/**
 * @brief Render the complete watch face into the full-screen frame buffer.
 *
 * Layout (logical coordinates, top-to-bottom):
 *   y=22  : Date string (YYYY-MM-DD), centered
 *   y=34  : Weekday string (MON/TUE/...), centered
 *   y=50  : Top separator line
 *   y=62  : HH:MM in medium 7-segment digits + SS in small 7-segment digits
 *   y=120 : Bottom separator line
 *   y=128 : NXP logo (80x39), centered
 */
static void Watch_RenderFull(uint8_t *framebuffer)
{
    /* Clear entire framebuffer */
    memset(framebuffer, 0x00, FRAME_BUFFER_SIZE);

    /* Draw date (top, centered) */
    char date_str[16];
    sprintf(date_str, "%04d-%02d-%02d", g_watchYear, g_watchMonth, g_watchDay);
    int date_width = (int)(strlen(date_str) * 6);
    DrawSmallString(framebuffer, (SCREEN_WIDTH - date_width) / 2, 22, date_str);

    /* Weekday, centered at y=34 */
    int weekday_width = (int)(strlen(g_weekdayStr[g_watchWeekday]) * 6);
    DrawSmallString(framebuffer, (SCREEN_WIDTH - weekday_width) / 2, 34, g_weekdayStr[g_watchWeekday]);

    /* Draw top separator line */
    DrawRect(framebuffer, 25, 50, 126, 1, 1);

    /* Draw time: HH:MM:SS */
    DrawMidDigit(framebuffer, TIME_START_X,      TIME_Y, g_watchHour   / 10u);
    DrawMidDigit(framebuffer, TIME_START_X + 27, TIME_Y, g_watchHour   % 10u);
    DrawColon   (framebuffer, TIME_START_X + 55, TIME_Y);
    DrawMidDigit(framebuffer, TIME_START_X + 61, TIME_Y, g_watchMinute / 10u);
    DrawMidDigit(framebuffer, TIME_START_X + 88, TIME_Y, g_watchMinute % 10u);
    DrawSmallDigit(framebuffer, SEC_X,      SEC_Y, g_watchSecond / 10u);
    DrawSmallDigit(framebuffer, SEC_X + 16, SEC_Y, g_watchSecond % 10u);

    /* Draw bottom separator line */
    DrawRect(framebuffer, 25, 120, 126, 1, 1);

    /* Draw NXP Logo (bottom, centered) */
    DrawNXPLogo(framebuffer, (SCREEN_WIDTH - NXP_LOGO_WIDTH) / 2, 128);
}

/** @brief Partial update: redraw seconds region into the dedicated seconds buffer. */
static void Watch_UpdateSeconds(void)
{
    /* Clear the dedicated seconds buffer */
    memset((void *)g_secondsBuffer, 0x00, SEC_BUFFER_SIZE);

    /* Draw seconds to the dedicated buffer using LOCAL coordinates (0,0 = top-left of buffer) */
    DrawSmallDigitRegion((uint8_t *)g_secondsBuffer,
                         SEC_REGION_BYTES_PER_ROW, SEC_REGION_WIDTH, SEC_REGION_HEIGHT,
                         0,  0, g_watchSecond / 10u);
    DrawSmallDigitRegion((uint8_t *)g_secondsBuffer,
                         SEC_REGION_BYTES_PER_ROW, SEC_REGION_WIDTH, SEC_REGION_HEIGHT,
                         16, 0, g_watchSecond % 10u);
}

/** @brief Partial update: redraw minutes digits in the main frame buffer. */
static void Watch_UpdateMinutes(uint8_t *framebuffer)
{
    ClearRect(framebuffer, TIME_START_X + 61, TIME_Y, 54, 49);
    DrawMidDigit(framebuffer, TIME_START_X + 61, TIME_Y, g_watchMinute / 10u);
    DrawMidDigit(framebuffer, TIME_START_X + 88, TIME_Y, g_watchMinute % 10u);
}

/** @brief Partial update: redraw hours digits in the main frame buffer. */
static void Watch_UpdateHours(uint8_t *framebuffer)
{
    ClearRect(framebuffer, TIME_START_X, TIME_Y, 54, 49);
    DrawMidDigit(framebuffer, TIME_START_X,      TIME_Y, g_watchHour / 10u);
    DrawMidDigit(framebuffer, TIME_START_X + 27, TIME_Y, g_watchHour % 10u);
}

/** @brief Partial update: redraw date and weekday in the main frame buffer. */
static void Watch_UpdateDate(uint8_t *framebuffer)
{
    char date_str[16];
    int  str_width;

    ClearRect(framebuffer, 0, 22, SCREEN_WIDTH, 20);

    sprintf(date_str, "%04d-%02d-%02d", g_watchYear, g_watchMonth, g_watchDay);
    str_width = (int)(strlen(date_str) * 6);
    DrawSmallString(framebuffer, (SCREEN_WIDTH - str_width) / 2, 22, date_str);

    str_width = (int)(strlen(g_weekdayStr[g_watchWeekday]) * 6);
    DrawSmallString(framebuffer, (SCREEN_WIDTH - str_width) / 2, 34, g_weekdayStr[g_watchWeekday]);
}

/******************************************************************************
 * SMARTDMA LCD Driver Functions
 *****************************************************************************/

/**
 * @brief Trigger a SmartDMA LCD refresh cycle.
 *
 * Sets the AHB clock divider to generate a ~2 MHz SmartDMA clock
 * (FRO12M / 6), then sets the EX bit in the SmartDMA control register
 * to start the display update.
 */
void CP_StartRefreshLcd(void)
{
    CLOCK_SetClockDiv(kCLOCK_DivAHBCLK, 6U);      /* 12 MHz / 6 = 2 MHz for SmartDMA */
    SMARTDMA0->CTRL = (0xC0DE0011u | (1u << 1u));  /* Set EX flag to start transfer    */
}

/**
 * @brief Poll until the SmartDMA LCD refresh cycle completes.
 * The EX bit is cleared by the SmartDMA firmware upon completion.
 */
void CP_WaitRefreshLcdDone(void)
{
    while (SMARTDMA0->CTRL & (1u << 1u));
}

/**
 * @brief Install the SmartDMA firmware and boot the engine with the parameter block.
 *
 * The prebuilt firmware image (g_tn0104SmartdmaFirmware) is first copied into
 * the SmartDMA private RAM (RAMX at TN0104_SMARTDMA_MEM_ADDR), then the EZH core
 * is booted from that address. This mirrors the MCUXpresso SDK SmartDMA drivers,
 * where SMARTDMA_InstallFirmware() memcpy's the firmware image into RAMX before
 * SMARTDMA_Boot() sets BOOTADR.
 *
 * @param param  Pointer to the LCD parameter block.
 */
void Tn0104_Boot(smartdma_lcd_param_t *param)
{
    /* Install firmware image into SmartDMA private RAM (RAMX) before boot */
    (void)memcpy((void *)TN0104_SMARTDMA_MEM_ADDR,
                 g_tn0104SmartdmaFirmware, g_tn0104SmartdmaFirmwareSize);

    SMARTDMA0->CTRL    |= (0xC0DE0000u | (1u << 4u));
    SMARTDMA0->ARM2EZH  = (uint32_t)param;
    SMARTDMA0->ARM2EZH |= 0x2u;
    SMARTDMA0->PENDTRAP = (0xFFu << 16u);
    SMARTDMA0->BOOTADR  = (uint32_t)TN0104_SMARTDMA_MEM_ADDR;   /* RAMX base address */
    SMARTDMA0->CTRL     = 0xC0DE0011u;
}

/**
 * @brief SMARTDMA interrupt handler.
 * Clears all pending trap flags on SmartDMA interrupt.
 */
void SMARTDMA_IRQHandler(void)
{
    SMARTDMA0->PENDTRAP = 0xFFu;
}

/******************************************************************************
 * WAKETIMER Functions (1 Hz timekeeping)
 *****************************************************************************/

/**
 * @brief WAKETIMER callback: update time, request display refresh, restart timer.
 * Called from WAKETIMER0_IRQHandler every second.
 */
static void WAKETIMER_Callback(void)
{
    Watch_UpdateTime();
    g_needRefresh = 1u;
    WAKETIMER_StartTimer(WAKETIMER0, WAKETIMER_1S_COUNT);
}

/**
 * @brief WAKETIMER0 interrupt handler.
 */
void WAKETIMER0_IRQHandler(void)
{
    WAKETIMER_ClearStatusFlags(WAKETIMER0, kWAKETIMER_WakeFlag);
    WAKETIMER_Callback();
}

/**
 * @brief Initialize WAKETIMER0 for 1 Hz periodic interrupts.
 *
 * Uses the FRO16K clock source. Timer reload value is calibrated to
 * WAKETIMER_1S_COUNT for a precise 1-second period.
 * Configures WUU to allow WAKETIMER to wake the MCU from Power Down mode.
 */
static void Timer_InitTimeUpdate(void)
{
    waketimer_config_t config;

    CLOCK_SetupFRO16KClocking(kCLKE_16K_SYSTEM);
    WAKETIMER_GetDefaultConfig(&config);
    WAKETIMER_Init(WAKETIMER0, &config);
    WAKETIMER_EnableInterrupts(WAKETIMER0, kWAKETIMER_WakeInterruptEnable);
    NVIC_SetPriority(WAKETIMER0_IRQn, 2);
    EnableIRQ(WAKETIMER0_IRQn);
    /* Enable WAKETIMER as wake-up source for Power Down mode via WUU */
    WUU_SetInternalWakeUpModulesConfig(WUU0, 2U, kWUU_InternalModuleInterrupt);
    WAKETIMER_StartTimer(WAKETIMER0, WAKETIMER_1S_COUNT);
}

/******************************************************************************
 * Low-Power Mode Functions
 *****************************************************************************/

/** @brief Enter Deep Sleep mode (core voltage at 1.0 V). */
static void APP_EnterDeepSleepMode(void)
{
    cmc_power_domain_config_t config;
    config.clock_mode  = kCMC_GateAllSystemClocksEnterLowPowerMode;
    config.main_domain = kCMC_DeepSleepMode;
    SPC_SetLowPowerModeCoreLDORegulatorVoltageLevel(SPC0, kSPC_CoreLDO_MidDriveVoltage);
    CMC_EnterLowPowerMode(CMC, &config);
}

/** @brief Enter Power Down mode (core voltage at 0.6 V retention). */
static void APP_EnterPowerDownMode(void)
{
    cmc_power_domain_config_t config;
    config.clock_mode  = kCMC_GateAllSystemClocksEnterLowPowerMode;
    config.main_domain = kCMC_PowerDownMode;
    SPC_SetLowPowerModeCoreLDORegulatorVoltageLevel(SPC0, kSPC_Core_LDO_RetentionVoltage);
    CMC_EnterLowPowerMode(CMC, &config);
}

/** @brief Configure all SRAM banks to be retained in low-power modes. */
static void APP_AllRamRetain(void)
{
    SPC0->LP_CFG           |= SPC_LP_CFG_SRAMLDO_DPD_ON_MASK;
    SPC0->SRAMRETLDO_CNTRL |= SPC_SRAMRETLDO_CNTRL_SRAM_RET_EN_MASK;
}

/** @brief Disable voltage detect */
static void APP_DisVoltageDetect(void)
{
    SPC_EnableActiveModeCoreLowVoltageDetect(SPC0, false);
    SPC_EnableActiveModeSystemHighVoltageDetect(SPC0, false);
    SPC_EnableActiveModeSystemLowVoltageDetect(SPC0, false);
}

/** @brief Re-enable voltage detect */
static void APP_EnVoltageDetect(void)
{
    SPC_EnableActiveModeCoreLowVoltageDetect(SPC0, true);
    SPC_EnableActiveModeSystemHighVoltageDetect(SPC0, true);
    SPC_EnableActiveModeSystemLowVoltageDetect(SPC0, true);
}

/**
 * @brief Configure SPC for low-power operation.
 *
 * Sets up active-mode and low-power-mode regulators, disables unused analog
 * modules, enables SRAM LDO, and configures the
 * low-power request output signal.
 */
static void APP_SetSPCConfiguration(void)
{
    status_t status;
    spc_active_mode_regulators_config_t activeModeRegulatorOption;

    /* Set low-power wake-up delay */
    SPC_SetLowPowerWakeUpDelay(SPC0, 0x5b);

    /* Enable SRAM LDO to retain SRAM contents in low-power modes */
    SPC_EnableSRAMLdo(SPC0, true);

    /* Disable all analog modules in active mode for power savings */
    SPC_DisableActiveModeAnalogModules(SPC0, kSPC_controlAllModules);
    APP_DisVoltageDetect();

    /* Configure active-mode regulators:
     * - Normal drive strength required when waking from Power Down */
    activeModeRegulatorOption.bandgapMode                        = kSPC_BandgapEnabledBufferDisabled;
    activeModeRegulatorOption.CoreLDOOption.CoreLDOVoltage       = kSPC_CoreLDO_MidDriveVoltage;
    activeModeRegulatorOption.CoreLDOOption.CoreLDODriveStrength = kSPC_CoreLDO_NormalDriveStrength;

    status = SPC_SetActiveModeRegulatorsConfig(SPC0, &activeModeRegulatorOption);

#if !(defined(FSL_FEATURE_MCX_SPC_HAS_NO_GLITCH_DETECT) && FSL_FEATURE_MCX_SPC_HAS_NO_GLITCH_DETECT)
    SPC_DisableActiveModeVddCoreGlitchDetect(SPC0, true);
#endif

    if (status != kStatus_Success) return;
    while (SPC_GetBusyStatusFlag(SPC0));

    SPC_DisableLowPowerModeAnalogModules(SPC0, kSPC_controlAllModules);

    spc_lowpower_mode_regulators_config_t lowPowerRegulatorOption;
    lowPowerRegulatorOption.lpIREF                               = false;
    lowPowerRegulatorOption.bandgapMode                          = kSPC_BandgapDisabled;
    lowPowerRegulatorOption.CoreLDOOption.CoreLDODriveStrength   = kSPC_CoreLDO_LowDriveStrength;

    status = SPC_SetLowPowerModeRegulatorsConfig(SPC0, &lowPowerRegulatorOption);

#if !(defined(FSL_FEATURE_MCX_SPC_HAS_NO_GLITCH_DETECT) && FSL_FEATURE_MCX_SPC_HAS_NO_GLITCH_DETECT)
    SPC_DisableLowPowerModeVddCoreGlitchDetect(SPC0, true);
#endif

    if (status != kStatus_Success) return;
    while (SPC_GetBusyStatusFlag(SPC0));

    spc_lowpower_request_config_t lpReqConfig = {
        .enable   = true,
        .override = kSPC_LowPowerRequestNotForced,
        .polarity = kSPC_LowTruePolarity,
    };
    SPC_SetLowPowerRequestConfig(SPC0, &lpReqConfig);
}

/**
 * @brief Configure CMC for low-power operation.
 * Disables low-power debug, allows all power modes, and configures flash
 * to enter low-power state automatically when the core clock is gated.
 */
static void APP_SetCMCConfiguration(void)
{
    CMC_EnableDebugOperation(CMC, false);
    CMC_SetPowerModeProtection(CMC, kCMC_AllowAllLowPowerModes);
    CMC_ConfigFlashMode(CMC, false, true, false);
}

/**
 * @brief Disable the FRO12M clock in Deep Sleep to reduce power consumption.
 * Unlocks and modifies SIRCCSR, then re-locks it.
 */
static void APP_DisFRO12M(void)
{
    SCG0->SIRCCSR &= ~SCG_SIRCCSR_LK_MASK;
    SCG0->SIRCCSR &= ~SCG_SIRCCSR_SIRCSTEN_MASK;
    SCG0->SIRCCSR &= ~SCG_SIRCCSR_SIRC_CLK_PERIPH_EN_MASK;
    SCG0->SIRCCSR |=  SCG_SIRCCSR_LK_MASK;
}

/**
 * @brief Pre-sleep hook: disable bandgap/detectors, retain RAM, restore AHB clock.
 * Called before entering low-power modes.
 */
static void APP_PowerPreSwitchHook(void)
{
    SPC_SetActiveModeBandgapModeConfig(SPC0, kSPC_BandgapDisabled);
    APP_DisVoltageDetect();
    APP_AllRamRetain();
    /* Restore full AHB clock speed to avoid pulsed current during transition */
    CLOCK_SetClockDiv(kCLOCK_DivAHBCLK, 1U);
}

/**
 * @brief Post-wake-up hook: restore bandgap and voltage detector configuration.
 * Called after returning from low-power modes.
 */
static void APP_PowerPostSwitchHook(void)
{
    SPC_SetActiveModeBandgapModeConfig(SPC0, kSPC_BandgapEnabledBufferDisabled);
    APP_EnVoltageDetect();
}

/**
 * @brief Enter the selected low-power mode.
 *
 * @param targetPowerMode  kAPP_PowerModeDeepSleep or kAPP_PowerModePowerDown.
 */
static void APP_PowerModeSwitch(app_power_mode_t targetPowerMode)
{
    switch (targetPowerMode)
    {
        case kAPP_PowerModeDeepSleep:
            APP_DisFRO12M();
            APP_EnterDeepSleepMode();
            break;
        case kAPP_PowerModePowerDown:
            APP_EnterPowerDownMode();
            break;
        default:
            assert(false);
            break;
    }
}

/******************************************************************************
 * Main Program
 *****************************************************************************/

/**
 * @brief Application entry point.
 *
 * Initialization sequence:
 *   1. Initialize board hardware (clocks, pins, debug console).
 *   2. Enable GPIO and SmartDMA clocks and resets.
 *   3. Boot SmartDMA with the TN0104 firmware and initial LCD parameters.
 *   4. Render the initial full watch face and perform the first LCD refresh.
 *   5. Configure power subsystems (SPC and CMC) once before the main loop.
 *   6. Start WAKETIMER for 1 Hz timekeeping (also configures WUU for Power Down).
 *
 * Main loop (low-power operation):
 *   - MCU enters the selected low-power mode (default: Power Down).
 *   - WAKETIMER wakes the MCU every 1 second via WUU.
 *   - On wake-up, the WAKETIMER ISR updates time and sets g_needRefresh.
 *   - Main loop performs smart partial LCD refresh:
 *       * Date changed   -> refresh date region  (framebuffer rows 133-153)
 *       * Hour/min changed -> refresh time region (framebuffer rows 65-113)
 *       * Second only    -> refresh seconds via g_secondsBuffer (4 bytes x 28 rows)
 *   - MCU re-enters low-power mode after each refresh cycle.
 */
int main(void)
{
    /* Initialize board hardware */
    BOARD_InitHardware();

    /* Enable GPIO clocks */
    CLOCK_EnableClock(kCLOCK_GateGPIO0);
    RESET_ReleasePeripheralReset(kGPIO0_RST_SHIFT_RSTn);
    CLOCK_EnableClock(kCLOCK_GateGPIO2);
    RESET_ReleasePeripheralReset(kGPIO2_RST_SHIFT_RSTn);

    /* Initialize SmartDMA */
    RESET_PeripheralReset(kSMART_DMA_RST_SHIFT_RSTn);
    CLOCK_EnableClock(kCLOCK_Smartdma);
    NVIC_EnableIRQ(SMARTDMA_IRQn);
    NVIC_SetPriority(SMARTDMA_IRQn, 3);

    /* Configure LCD parameter block for full-screen transfer */
    g_lcdParam.p_data = (uint32_t *)g_frameBuffer;
    g_lcdParam.v_addr = 0;    /* Vertical start (row 0)                      */
    g_lcdParam.h_addr = 0;    /* Horizontal start (byte 0)                   */
    g_lcdParam.h_len  = BYTES_PER_ROW;  /* 22 bytes = 176 pixels per row     */
    g_lcdParam.v_len  = SCREEN_HEIGHT;  /* 176 rows                          */

    /* Boot SmartDMA with the TN0104 firmware */
    Tn0104_Boot(&g_lcdParam);

    /* Render and display the initial watch face */
    Watch_RenderFull((uint8_t *)g_frameBuffer);
    CP_StartRefreshLcd();
    CP_WaitRefreshLcdDone();

    /* Configure power subsystems once (settings persist across sleep cycles) */
    APP_SetSPCConfiguration();
    APP_SetCMCConfiguration();

    /* Start WAKETIMER for 1 Hz timekeeping (also enables WUU for Power Down) */
    Timer_InitTimeUpdate();

    /* Shadow copies of time fields for change detection */
    uint8_t prev_second = g_watchSecond;
    uint8_t prev_minute = g_watchMinute;
    uint8_t prev_hour   = g_watchHour;
    uint8_t prev_day    = g_watchDay;

    /* Main loop ------------------------------------------------------------ */
    while (1)
    {
        if (g_needRefresh)
        {
            g_needRefresh = 0u;

            uint8_t need_date_refresh     = 0u;
            uint8_t need_hour_min_refresh = 0u;
            uint8_t need_second_refresh   = 0u;

            /* Detect what changed and update only affected frame buffer areas */
            if (prev_day != g_watchDay)
            {
                Watch_UpdateDate((uint8_t *)g_frameBuffer);
                prev_day = g_watchDay;
                need_date_refresh = 1u;
            }
            if (prev_hour != g_watchHour)
            {
                Watch_UpdateHours((uint8_t *)g_frameBuffer);
                prev_hour = g_watchHour;
                need_hour_min_refresh = 1u;
            }
            if (prev_minute != g_watchMinute)
            {
                Watch_UpdateMinutes((uint8_t *)g_frameBuffer);
                prev_minute = g_watchMinute;
                need_hour_min_refresh = 1u;
            }
            if (prev_second != g_watchSecond)
            {
                Watch_UpdateSeconds();
                prev_second = g_watchSecond;
                need_second_refresh = 1u;
            }

            /* --- Partial LCD refresh --- */
            if (need_date_refresh)
            {
                /* Refresh date area: framebuffer rows 133-153
                 * (logical y=22..41 maps to stored rows 134..155 after flip) */
                g_lcdParam.p_data = (uint32_t *)(g_frameBuffer + 133u * BYTES_PER_ROW);
                g_lcdParam.v_addr = 133;
                g_lcdParam.h_addr = 0;
                g_lcdParam.h_len  = BYTES_PER_ROW;
                g_lcdParam.v_len  = 21;
                SMARTDMA0->ARM2EZH = (uint32_t)&g_lcdParam;
                CP_StartRefreshLcd();
                CP_WaitRefreshLcdDone();
            }
            if (need_hour_min_refresh)
            {
                /* Refresh time area: framebuffer rows 65-113 (logical y=62..111) */
                g_lcdParam.p_data = (uint32_t *)(g_frameBuffer + 65u * BYTES_PER_ROW);
                g_lcdParam.v_addr = 65;
                g_lcdParam.h_addr = 0;
                g_lcdParam.h_len  = BYTES_PER_ROW;
                g_lcdParam.v_len  = 49;
                SMARTDMA0->ARM2EZH = (uint32_t)&g_lcdParam;
                CP_StartRefreshLcd();
                CP_WaitRefreshLcdDone();
            }
            else if (need_second_refresh)
            {
                /* Refresh seconds region only using the dedicated partial buffer.
                 * Coordinate mapping (after 180-degree flip):
                 *   Logical SEC_X=136, SEC_Y=70, region 32x28 px
                 *   -> v_addr = 175 - (70+28-1) = 78
                 *   -> h_addr = 175 - (136+32-1) = 8  (8-pixel/byte aligned)
                 */
                g_lcdParam.p_data = (uint32_t *)g_secondsBuffer;
                g_lcdParam.v_addr = 78;
                g_lcdParam.h_addr = 8;
                g_lcdParam.h_len  = SEC_REGION_BYTES_PER_ROW;
                g_lcdParam.v_len  = SEC_REGION_HEIGHT;
                SMARTDMA0->ARM2EZH = (uint32_t)&g_lcdParam;
                CP_StartRefreshLcd();
                CP_WaitRefreshLcdDone();
            }
        }

        /* Prepare for low-power entry and enter the selected mode */
        APP_PowerPreSwitchHook();
        APP_PowerModeSwitch(g_targetPowerMode);

        /* Restore system configuration after wake-up */
        APP_PowerPostSwitchHook();
    }
}
