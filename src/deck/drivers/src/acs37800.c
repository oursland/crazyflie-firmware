/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie control firmware
 *
 * Copyright (C) 2021 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * asc37800.c - Deck driver for ACS37800 power monitoring IC
 */

#include <stdint.h>
#include <stdlib.h>
#include "stm32fxxx.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "debug.h"
#include "system.h"
#include "deck.h"
#include "param.h"
#include "log.h"
#include "sleepus.h"
#include "i2cdev.h"


// EEPROM
#define ACSREG_E_TRIM         0x0B
#define ACSREG_E_OFFS_AVG     0x0C
#define ACSREG_E_FAULT        0x0D
#define ACSREG_E_UNDR_OVR_V   0x0E
#define ACSREG_E_IO_SEL       0x0F

// SHADOW
#define ACSREG_S_TRIM         0x1B
#define ACSREG_S_OFFS_AVG     0x1C
#define ACSREG_S_FAULT        0x1D
#define ACSREG_S_UNDR_OVR_V   0x1E
#define ACSREG_S_IO_SEL       0x1F

#define ACSREG_I_V_RMS      0x20
#define ACSREG_P_ACTIVE     0x21
#define ACSREG_P_APPARENT   0x22
#define ACSREG_NUM_PTS_OUT  0x25
#define ACSREG_VRMS_AVGS    0x26
#define ACSREG_VRMS_AVGM    0x27
#define ACSREG_P_ACT_AVGS   0x28
#define ACSREG_P_ACT_AVGM   0x29
#define ACSREG_I_V_CODES    0x2A
#define ACSREG_P_INSTANT    0x2C
#define ACSREG_DSP_STATUS   0x2D
#define ACSREG_ACCESS_CODE  0x2F

#define ACS_I2C_ADDR        0x7F
#define ACS_ACCESS_CODE     0x4F70656E


static bool isInit;
static uint32_t viBatRaw;
static uint32_t viBatRMS;
static uint32_t vavgBatRMS;
static float vBat;
static float vBatRMS;
static float vavgBat;
static float iBat;
static float iBatRMS;
static float iavgBat;
static uint32_t pBatRaw;
static uint32_t pavgBatRaw;
static float pBat;
static float pavgBat;

static uint16_t currZtrim, currZtrimOld;

static void asc37800Task(void* prm);

/*
 * Sign extend a bitfield which if right justified
 *
 *    data        - the bitfield to be sign extended
 *    width       - the width of the bitfield
 *    returns     - the sign extended bitfield
 */
int32_t signExtendBitfield(uint32_t data, uint16_t width)
{
  // If the bitfield is the width of the variable, don't bother trying to sign extend (it already is)
    if (width == 32)
    {
        return (int32_t)data;
    }

    int32_t x = (int32_t)data;
    int32_t mask = 1L << (width - 1);

    x = x & ((1 << width) - 1); // make sure the upper bits are zero

    return (int32_t)((x ^ mask) - mask);
}

/*
 * Convert an unsigned bitfield which is right justified, into a floating point number
 *
 *    data        - the bitfield to be converted
 *    binaryPoint - the binary point (the bit to the left of the binary point)
 *    width       - the width of the bitfield
 *    returns     - the floating point number
 */
float convertUnsignedFixedPoint(uint32_t inputValue, uint16_t binaryPoint, uint16_t width)
{
    uint32_t mask;

    if (width == 32)
    {
        mask = 0xFFFFFFFF;
    }
    else
    {
        mask = (1UL << width) - 1UL;
    }

    return (float)(inputValue & mask) / (float)(1L << binaryPoint);
}

/*
 * Convert a signed bitfield which is right justified, into a floating point number
 *
 *    data        - the bitfield to be sign extended then converted
 *    binaryPoint - the binary point (the bit to the left of the binary point)
 *    width       - the width of the bitfield
 *    returns     - the floating point number
 */
float convertSignedFixedPoint(uint32_t inputValue, uint16_t binaryPoint, uint16_t width)
{
    int32_t signedValue = signExtendBitfield(inputValue, width);
    return (float)signedValue / (float)(1L << binaryPoint);
}

static bool asc37800Read32(uint8_t reg, uint32_t *data32)
{
  return i2cdevReadReg8(I2C1_DEV, ACS_I2C_ADDR, reg, 4, (uint8_t *)data32);
}

static bool asc37800Write32(uint8_t reg, uint32_t data32)
{
  return i2cdevWriteReg8(I2C1_DEV, ACS_I2C_ADDR, reg, 4, (uint8_t *)&data32);
}

static void ascFindAndSetAddress(void)
{
  uint8_t startAddr;
  uint32_t dummy = 0;

  for (startAddr = 96; startAddr <= 110; startAddr++)
  {
    bool isReplying = i2cdevWrite(I2C1_DEV, startAddr, 1, (uint8_t *)&dummy);

    if (isReplying)
    {
      // Unlock EEPROM
      dummy = ACS_ACCESS_CODE;
      i2cdevWriteReg8(I2C1_DEV, startAddr, ACSREG_ACCESS_CODE, 4, (uint8_t *)&dummy);
      // EEPROM: write and lock i2c address to 0x7F;
      i2cdevReadReg8(I2C1_DEV, startAddr, ACSREG_E_IO_SEL, 4, (uint8_t *)&dummy);
      DEBUG_PRINT("ACS37800 A:0x%.2X R:0x%.2X:%X\n", (unsigned int)startAddr, (unsigned int)ACSREG_E_IO_SEL, (unsigned int)dummy);

      dummy = (0x7F << 2) | (0x01 << 9);
      i2cdevWriteReg8(I2C1_DEV, startAddr, ACSREG_E_IO_SEL, 4, (uint8_t *)&dummy);
      vTaskDelay(M2T(10));

      DEBUG_PRINT("ACS37800 found on: %d. Setting address to 0x7E. Power cycle needed!\n", startAddr);
    }
  }
}


static void asc37800Init(DeckInfo *info)
{
  uint8_t dummy;
  uint32_t val;

  if (isInit) {
    return;
  }

  if (i2cdevWrite(I2C1_DEV, ACS_I2C_ADDR, 1, (uint8_t *)&dummy))
  {
    asc37800Write32(ACSREG_ACCESS_CODE, ACS_ACCESS_CODE);
    DEBUG_PRINT("ACS37800 I2C [OK]\n");
  }
  else
  {
    ascFindAndSetAddress();
  }

  // Enable bypass in shadow reg and set N to 32.
  asc37800Write32(ACSREG_S_IO_SEL, (1 << 24) | (32 << 14) | (0x01 << 9) |  (0x7F << 2));
  // Set current and power for averaging and keep device specific (trim) settings.
  asc37800Read32(ACSREG_S_TRIM, &val);
  currZtrim = currZtrimOld = val & 0xFF;
  asc37800Write32(ACSREG_S_TRIM, (val | (1 << 23) | (1 << 22)));
  // Set average to 10 samples. (ASC37800 sample rate 1khz)
  asc37800Write32(ACSREG_S_OFFS_AVG, ((10 << 7) | (10 << 0)));

  DEBUG_PRINT("---------------\n");
  for (int reg = 0x0B; reg <= 0x0F; reg++)
  {
    bool res;

    res = asc37800Read32(reg, &val);
    DEBUG_PRINT("%d:R:0x%.2X:%X\n", (unsigned int)res, (unsigned int)reg, (unsigned int)val);
    res = asc37800Read32(reg+0x10, &val);
    DEBUG_PRINT("%d:R:0x%.2X:%X\n\n", (unsigned int)res, (unsigned int)reg+0x10, (unsigned int)val);
  }


  xTaskCreate(asc37800Task, "asc37800",
              2*configMINIMAL_STACK_SIZE, NULL,
              /*priority*/2, NULL);

  isInit = true;
}

static void asc37800Task(void* prm)
{
  systemWaitStart();

  TickType_t lastWakeTime = xTaskGetTickCount();

  while(1) {
    vTaskDelayUntil(&lastWakeTime, M2T(10));

    asc37800Read32(ACSREG_I_V_CODES, &viBatRaw);
    asc37800Read32(ACSREG_I_V_RMS, &viBatRMS);
    asc37800Read32(ACSREG_VRMS_AVGS, &vavgBatRMS);
    asc37800Read32(ACSREG_P_INSTANT, &pBatRaw);
    asc37800Read32(ACSREG_P_ACT_AVGS, &pavgBatRaw);

    vBat = (int16_t)(viBatRaw & 0xFFFF) / 27500.0 * 0.250 * 92;
    iBat = (int16_t)(viBatRaw >> 16 & 0xFFFF) / 27500.0 * 30.0;
    vBatRMS = (uint16_t)(viBatRMS & 0xFFFF) / 55000.0 * 0.250 * 92;
    iBatRMS = (uint16_t)(viBatRMS >> 16 & 0xFFFF) / 55000.0 * 30.0;
    vavgBat = (uint16_t)(vavgBatRMS & 0xFFFF) / 55000.0 * 0.250 * 92;
    iavgBat = (uint16_t)(vavgBatRMS >> 16 & 0xFFFF) / 55000.0 * 30.0;
    pBat = (int16_t)(pBatRaw & 0xFFFF) / 3.08 * 92 / 1000.0;
    pavgBat = (int16_t)(pavgBatRaw & 0xFFFF) / 3.08 * 92 / 1000.0;

    if (currZtrimOld != currZtrim)
    {
      uint32_t val;
      asc37800Read32(ACSREG_S_TRIM, &val);
      DEBUG_PRINT("%X ->", (unsigned int)val);
      val = (val & 0xFFFFFE00) | currZtrim;
      DEBUG_PRINT("%X\n ", (unsigned int)val);
      asc37800Write32(ACSREG_S_TRIM, val);
      currZtrimOld = currZtrim;
    }

//    DEBUG_PRINT("V: %.3f I: %.3f P: %.3f\n", vBat, iBat, pBat);
//    DEBUG_PRINT("V: %f\tI: %f\tP: %f\n", vBat, iBat, pBat);
//    DEBUG_PRINT("Vc: %f\tIc: %f\tV: %f\tI: %f\tP: %f\n", vavgBat, iavgBat, vBat, iBat, pBat);
  }
}

static const DeckDriver asc37800_deck = {
  .vid = 0x00,
  .pid = 0x00,
  .name = "bcACS37800",

  .usedGpio = DECK_USING_SCL | DECK_USING_SDA,

  .init = asc37800Init,
};

DECK_DRIVER(asc37800_deck);

PARAM_GROUP_START(deck)
PARAM_ADD(PARAM_UINT8 | PARAM_RONLY, bcACS37800, &isInit)
PARAM_GROUP_STOP(deck)

PARAM_GROUP_START(asc37800)
PARAM_ADD(PARAM_UINT16, currZtrim, &currZtrim)
PARAM_GROUP_STOP(asc37800)

LOG_GROUP_START(asc37800)
LOG_ADD(LOG_FLOAT, v, &vBat)
LOG_ADD(LOG_FLOAT, vRMS, &vBatRMS)
LOG_ADD(LOG_FLOAT, v_avg, &vavgBat)
LOG_ADD(LOG_FLOAT, i, &iBat)
LOG_ADD(LOG_FLOAT, iRMS, &iBatRMS)
LOG_ADD(LOG_FLOAT, i_avg, &iavgBat)
LOG_ADD(LOG_FLOAT, p, &pBat)
LOG_ADD(LOG_FLOAT, p_avg, &pavgBat)
LOG_GROUP_STOP(asc37800)
