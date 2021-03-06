/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Register map for STM32 processor
 */

#ifndef __CROS_EC_REGISTERS_H
#define __CROS_EC_REGISTERS_H

#include "common.h"

/*
 *  Peripheral IDs
 *
 *  nRF51 has very good design that the peripheral IDs is actually the IRQ#.
 *  Thus, the following numbers are used in DECLARE_IRQ(), task_enable_irq()
 *  and task_disable_irq().
 */
#define NRF51_PERID_POWER   0
#define NRF51_PERID_CLOCK   0
#define NRF51_PERID_RADIO   1
#define NRF51_PERID_USART   2
#define NRF51_PERID_SPI0    3
#define NRF51_PERID_TWI0    3
#define NRF51_PERID_SPI1    4
#define NRF51_PERID_TWI1    4
#define NRF51_PERID_SPIS    4
#define NRF51_PERID_GPIOTE  6
#define NRF51_PERID_ADC     7
#define NRF51_PERID_TIMER0  8
#define NRF51_PERID_TIMER1  9
#define NRF51_PERID_TIMER2  10
#define NRF51_PERID_RTC     11
#define NRF51_PERID_TEMP    12
#define NRF51_PERID_RNG     13
#define NRF51_PERID_ECB     14
#define NRF51_PERID_CCM     15
#define NRF51_PERID_AAR     16
#define NRF51_PERID_WDT     17
#define NRF51_PERID_QDEC    18
#define NRF51_PERID_LPCOMP  19
#define NRF51_PERID_NVMC    30
#define NRF51_PERID_PPI     31


/*
 *  Power
 */
#define NRF51_POWER_BASE    0x40000000
/* Tasks */
#define NRF51_POWER_CONSTLAT  REG32(NRF51_POWER_BASE + 0x078)
#define NRF51_POWER_LOWPWR    REG32(NRF51_POWER_BASE + 0x07c)
/* Events */
#define NRF51_POWER_POFWARN   REG32(NRF51_POWER_BASE + 0x108)
/* Registers */
#define NRF51_POWER_INTENSET  REG32(NRF51_POWER_BASE + 0x304)
#define NRF51_POWER_INTENCLR  REG32(NRF51_POWER_BASE + 0x308)
#define NRF51_POWER_RESETREAS REG32(NRF51_POWER_BASE + 0x400)
#define NRF51_POWER_SYSTEMOFF REG32(NRF51_POWER_BASE + 0x500)
#define NRF51_POWER_POFCON    REG32(NRF51_POWER_BASE + 0x510)
#define NRF51_POWER_GPREGRET  REG32(NRF51_POWER_BASE + 0x51c)
#define NRF51_POWER_RAMON     REG32(NRF51_POWER_BASE + 0x524)
#define NRF51_POWER_RESET     REG32(NRF51_POWER_BASE + 0x544)
#define NRF51_POWER_DCDCEN    REG32(NRF51_POWER_BASE + 0x578)


/*
 *  Clock
 */
#define NRF51_CLOCK_BASE    0x40000000
/* Tasks */
#define NRF51_CLOCK_HFCLKSTART REG32(NRF51_CLOCK_BASE + 0x000)
#define NRF51_CLOCK_HFCLKSTOP  REG32(NRF51_CLOCK_BASE + 0x004)
#define NRF51_CLOCK_LFCLKSTART REG32(NRF51_CLOCK_BASE + 0x008)
#define NRF51_CLOCK_LFCLKSTOP  REG32(NRF51_CLOCK_BASE + 0x00c)
#define NRF51_CLOCK_CAL     REG32(NRF51_CLOCK_BASE + 0x010)
#define NRF51_CLOCK_CTSTART REG32(NRF51_CLOCK_BASE + 0x014)
#define NRF51_CLOCK_CTSTOP  REG32(NRF51_CLOCK_BASE + 0x018)
/* Events */
#define NRF51_CLOCK_HFCLKSTARTED REG32(NRF51_CLOCK_BASE + 0x100)
#define NRF51_CLOCK_LFCLKSTARTED REG32(NRF51_CLOCK_BASE + 0x104)
#define NRF51_CLOCK_DONE    REG32(NRF51_CLOCK_BASE + 0x10c)
#define NRF51_CLOCK_CCTO    REG32(NRF51_CLOCK_BASE + 0x110)
/* Registers */
#define NRF51_CLOCK_INTENSET  REG32(NRF51_CLOCK_BASE + 0x304)
#define NRF51_CLOCK_INTENCLR  REG32(NRF51_CLOCK_BASE + 0x308)
#define NRF51_CLOCK_HFCLKSTAT REG32(NRF51_CLOCK_BASE + 0x40c)
#define NRF51_CLOCK_LFCLKSTAT REG32(NRF51_CLOCK_BASE + 0x418)
#define NRF51_CLOCK_LFCLKSRC  REG32(NRF51_CLOCK_BASE + 0x518)
#define NRF51_CLOCK_CTIV      REG32(NRF51_CLOCK_BASE + 0x538)
#define NRF51_CLOCK_XTALFREQ  REG32(NRF51_CLOCK_BASE + 0x550)

/*
 *  UART
 */
#define NRF51_UART_BASE     0x40002000
/* Tasks */
#define NRF51_UART_STARTRX  REG32(NRF51_UART_BASE + 0x000)
#define NRF51_UART_STOPRX   REG32(NRF51_UART_BASE + 0x004)
#define NRF51_UART_STARTTX  REG32(NRF51_UART_BASE + 0x008)
#define NRF51_UART_STOPTX   REG32(NRF51_UART_BASE + 0x00c)
/* Events */
#define NRF51_UART_RXDRDY   REG32(NRF51_UART_BASE + 0x108)
#define NRF51_UART_TXDRDY   REG32(NRF51_UART_BASE + 0x11c)
#define NRF51_UART_ERROR    REG32(NRF51_UART_BASE + 0x124)
#define NRF51_UART_RXTO     REG32(NRF51_UART_BASE + 0x144)
/* Registers */
#define NRF51_UART_INTENSET REG32(NRF51_UART_BASE + 0x304)
#define NRF51_UART_INTENCLR REG32(NRF51_UART_BASE + 0x308)
#define NRF51_UART_ERRORSRC REG32(NRF51_UART_BASE + 0x480)
#define NRF51_UART_ENABLE   REG32(NRF51_UART_BASE + 0x500)
#define NRF51_UART_PSELRTS  REG32(NRF51_UART_BASE + 0x508)
#define NRF51_UART_PSELTXD  REG32(NRF51_UART_BASE + 0x50c)
#define NRF51_UART_PSELCTS  REG32(NRF51_UART_BASE + 0x510)
#define NRF51_UART_PSELRXD  REG32(NRF51_UART_BASE + 0x514)
#define NRF51_UART_RXD      REG32(NRF51_UART_BASE + 0x518)
#define NRF51_UART_TXD      REG32(NRF51_UART_BASE + 0x51C)
#define NRF51_UART_BAUDRATE REG32(NRF51_UART_BASE + 0x524)
#define NRF51_UART_CONFIG   REG32(NRF51_UART_BASE + 0x56c)
/* For UART.INTEN bits */
#define NRF55_UART_RXDRDY_BIT  ((0x108 - 0x100) / 4)
#define NRF55_UART_TXDRDY_BIT  ((0x11c - 0x100) / 4)


/*
 *  Timer / Counter
 */
#define NRF51_TIMER0_BASE   0x40008000
/* Tasks */
#define NRF51_TIMER0_START  REG32(NRF51_TIMER0_BASE + 0x000)
#define NRF51_TIMER0_STOP   REG32(NRF51_TIMER0_BASE + 0x004)
#define NRF51_TIMER0_COUNT  REG32(NRF51_TIMER0_BASE + 0x008)
#define NRF51_TIMER0_CLEAR  REG32(NRF51_TIMER0_BASE + 0x00c)
#define NRF51_TIMER0_CAPTURE0  REG32(NRF51_TIMER0_BASE + 0x040)
#define NRF51_TIMER0_CAPTURE1  REG32(NRF51_TIMER0_BASE + 0x044)
#define NRF51_TIMER0_CAPTURE2  REG32(NRF51_TIMER0_BASE + 0x048)
#define NRF51_TIMER0_CAPTURE3  REG32(NRF51_TIMER0_BASE + 0x04c)
/* Events */
#define NRF51_TIMER0_COMPARE0  REG32(NRF51_TIMER0_BASE + 0x140)
#define NRF51_TIMER0_COMPARE1  REG32(NRF51_TIMER0_BASE + 0x144)
#define NRF51_TIMER0_COMPARE2  REG32(NRF51_TIMER0_BASE + 0x148)
#define NRF51_TIMER0_COMPARE3  REG32(NRF51_TIMER0_BASE + 0x14c)
/* Registers */
#define NRF51_TIMER0_SHORTCUT  REG32(NRF51_TIMER0_BASE + 0x200)
#define NRF51_TIMER0_INTENSET  REG32(NRF51_TIMER0_BASE + 0x304)
#define NRF51_TIMER0_INTENCLR  REG32(NRF51_TIMER0_BASE + 0x308)
#define NRF51_TIMER0_MODE      REG32(NRF51_TIMER0_BASE + 0x504)
#define NRF51_TIMER0_BITMODE   REG32(NRF51_TIMER0_BASE + 0x508)
#define NRF51_TIMER0_PRESCALER REG32(NRF51_TIMER0_BASE + 0x510)
#define NRF51_TIMER0_CC0       REG32(NRF51_TIMER0_BASE + 0x540)
#define NRF51_TIMER0_CC1       REG32(NRF51_TIMER0_BASE + 0x544)
#define NRF51_TIMER0_CC2       REG32(NRF51_TIMER0_BASE + 0x548)
#define NRF51_TIMER0_CC3       REG32(NRF51_TIMER0_BASE + 0x54c)
/* For Timer.INTEN bits */
#define NRF51_TIMER_COMPARE0_BIT  ((0x140 - 0x100) / 4)
#define NRF51_TIMER_COMPARE1_BIT  ((0x144 - 0x100) / 4)
#define NRF51_TIMER_COMPARE2_BIT  ((0x148 - 0x100) / 4)
#define NRF51_TIMER_COMPARE3_BIT  ((0x14c - 0x100) / 4)
/* For Timer Shortcuts */
#define NRF51_TIMER_COMPARE0_CLEAR  (1 << 0)
#define NRF51_TIMER_COMPARE1_CLEAR  (1 << 1)
#define NRF51_TIMER_COMPARE2_CLEAR  (1 << 2)
#define NRF51_TIMER_COMPARE3_CLEAR  (1 << 3)
#define NRF51_TIMER_COMPARE0_STOP   (1 << 8)
#define NRF51_TIMER_COMPARE1_STOP   (1 << 9)
#define NRF51_TIMER_COMPARE2_STOP   (1 << 10)
#define NRF51_TIMER_COMPARE3_STOP   (1 << 11)
/* Timer Mode (NRF51_TIMER0_MODE) */
#define NRF51_TIMER0_MODE_TIMER   0  /* reset default */
#define NRF51_TIMER0_MODE_COUNTER 0
/* Prescaler */
#define NRF51_TIMER0_PRESCALER_MASK (0xf)  /* range: 0-9, reset default: 4 */
/* Bit length (NRF51_TIMER0_BITMODE) */
#define NRF51_TIMER0_BITMODE_16  0  /* reset default */
#define NRF51_TIMER0_BITMODE_8   1
#define NRF51_TIMER0_BITMODE_24  2
#define NRF51_TIMER0_BITMODE_32  3


/*
 *  GPIO
 */
#define NRF51_GPIO_BASE     0x50000000
#define NRF51_GPIO0_BASE    (NRF51_GPIO_BASE + 0x500)
#define NRF51_GPIO0_OUT     REG32(NRF51_GPIO0_BASE + 0x004)
#define NRF51_GPIO0_OUTSET  REG32(NRF51_GPIO0_BASE + 0x008)
#define NRF51_GPIO0_OUTCLR  REG32(NRF51_GPIO0_BASE + 0x00c)
#define NRF51_GPIO0_IN      REG32(NRF51_GPIO0_BASE + 0x010)
#define NRF51_GPIO0_DIR     REG32(NRF51_GPIO0_BASE + 0x014)  /* 1 for output */
#define NRF51_GPIO0_DIRSET  REG32(NRF51_GPIO0_BASE + 0x018)
#define NRF51_GPIO0_DIRCLR  REG32(NRF51_GPIO0_BASE + 0x01c)
#define NRF51_PIN_BASE      (NRF51_GPIO_BASE + 0x700)
#define NRF51_PIN_CNF(n)    REG32(NRF51_PIN_BASE + ((n) * 4))
#define GPIO_0              NRF51_GPIO0_BASE
#define DUMMY_GPIO_BANK     GPIO_0  /* for UNIMPLEMENTED() macro */


#endif /* __CROS_EC_REGISTERS_H */

