// HC32F460 serial
//
// Copyright (C) 2022  Steven Gotthardt <gotthardt@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_SERIAL_BAUD

#include "command.h"    // DECL_CONSTANT_STR
#include "internal.h"
#include "sched.h"      // DECL_INIT
#include "generic/serial_irq.h"
#include "generic/armcm_boot.h"

#include "hc32f460_usart.h"
#include "hc32f460_gpio.h"
#include "hc32f460_pwc.h"


#define USART_BAUDRATE  (CONFIG_SERIAL_BAUD)


// Aquila 1.0.3 pins for TX, RX to CH304 on PA15 and PA09
//    for the LCD connector: TX, RX on PC00 and PC01
#if CONFIG_HC32F460_SERIAL_PA15_PA9
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA15,PA9");
  #define USART_RX_PORT   (PortA)
  #define USART_RX_PIN    (Pin15)
  #define USART_TX_PORT   (PortA)
  #define USART_TX_PIN    (Pin09)

#elif CONFIG_HC32F460_SERIAL_PC0_PC1
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PC0,PC1");
  #define USART_RX_PORT   (PortC)
  #define USART_RX_PIN    (Pin00)
  #define USART_TX_PORT   (PortC)
  #define USART_TX_PIN    (Pin01)

#elif CONFIG_HC32F460_SERIAL_PA3_PA2
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA3,PA2");
  #define USART_RX_PORT   (PortA)
  #define USART_RX_PIN    (Pin03)
  #define USART_TX_PORT   (PortA)
  #define USART_TX_PIN    (Pin02)
#endif

// use USART 1 for serial connection
#define USARTx          M4_USART1
#define USART_ENABLE    PWC_FCG1_PERIPH_USART1 | PWC_FCG1_PERIPH_USART2 | \
                        PWC_FCG1_PERIPH_USART3 | PWC_FCG1_PERIPH_USART4

#define USART_RX_FUNC   Func_Usart1_Rx
#define USART_TX_FUNC   Func_Usart1_Tx
#define USART_RI_NUM    INT_USART1_RI
#define USART_TI_NUM    INT_USART1_TI
#define USART_EI_NUM    INT_USART1_EI
#define USART_TCI_NUM   INT_USART1_TCI


void
serialError(void)
{
    if (Set == USART_GetStatus(USARTx, UsartFrameErr))
    {
        USART_ClearStatus(USARTx, UsartFrameErr);

        // use it anyway
        serial_rx_byte(USARTx->DR_f.RDR);
    }

    if (Set == USART_GetStatus(USARTx, UsartOverrunErr))
    {
        USART_ClearStatus(USARTx, UsartOverrunErr);
    }
}
DECL_ARMCM_IRQ(serialError, Int001_IRQn);


void
serialTxComplete(void)
{
    USART_FuncCmd(USARTx, UsartTx, Disable);
    USART_FuncCmd(USARTx, UsartTxCmpltInt, Disable);
}
DECL_ARMCM_IRQ(serialTxComplete, Int003_IRQn);


void
serialRx(void)
{
    uint16_t data = USART_RecData(USARTx);

    // call to klipper generic/serial_irq function
    serial_rx_byte(data);
}
DECL_ARMCM_IRQ(serialRx, Int000_IRQn);


void
serialTxEmpty(void)
{
    uint8_t data2send;

    // use helper from generic - zero means byte ready
    if (!serial_get_tx_byte(&data2send))
    {
        //USARTx->DR_f.TDR = data2send;
        USART_SendData(USARTx, data2send);
    }
    else
    {
        // no more data - stop the interrupt
        USART_FuncCmd(USARTx, UsartTxCmpltInt, Enable);
        USART_FuncCmd(USARTx, UsartTxEmptyInt, Disable);
    }
}
DECL_ARMCM_IRQ(serialTxEmpty, Int002_IRQn);


// called by generic framework
void
serial_enable_tx_irq(void)
{
    /* Enable TX && TX empty interrupt */
    USART_FuncCmd(USARTx, UsartTxAndTxEmptyInt, Enable);
}


void
serial_init(void)
{
    const stc_usart_uart_init_t stcInitCfg = {
        UsartIntClkCkNoOutput,
        UsartClkDiv_1,
        UsartDataBits8,
        UsartDataLsbFirst,
        UsartOneStopBit,
        UsartParityNone,
        UsartSampleBit16,
        UsartStartBitFallEdge,
        UsartRtsEnable,
    };

    // Function Clock Control - USART enable (sets bit to a 0)
    PWC_Fcg1PeriphClockCmd(USART_ENABLE, Enable);

    /* Initialize port pins for USART IO - Disable = NO subfunction */
    PORT_SetFunc(USART_RX_PORT, USART_RX_PIN, USART_RX_FUNC, Disable);
    PORT_SetFunc(USART_TX_PORT, USART_TX_PIN, USART_TX_FUNC, Disable);

    /* Initialize UART */
    USART_UART_Init(USARTx, &stcInitCfg);

    /* Set baudrate */
    USART_SetBaudrate(USARTx, USART_BAUDRATE);

  /* A word on interrupts in HC32F460
    In other vendors (STM) the irqs are assigned names in the vector list
      eg: USART1_IRQn but HC32F460 has numbered IRQs: IRQ000_IRQn IRQ143_IRQn
      Using INT_USART1_RI from hc32f460.h put in to IRQn->INTSEL
       if n = 5 then the USART1_RI iterrupt will be at IRQ005_IRQn
       For the specific case of each USART there are 5 separate irqs to map
  */

    /* Set USART RX IRQ */
    IrqRegistration(USART_RI_NUM,  Int000_IRQn);

    /* Set USART err */
    IrqRegistration(USART_EI_NUM,  Int001_IRQn);

    /* Set USART TX IRQ */
    IrqRegistration(USART_TI_NUM,  Int002_IRQn);

    /* Set USART TX complete IRQ */
    IrqRegistration(USART_TCI_NUM, Int003_IRQn);

    /* Enable RX && RX interrupt function */
    USART_FuncCmd(USARTx, UsartRx, Enable);
    USART_FuncCmd(USARTx, UsartRxInt, Enable);
}

DECL_INIT(serial_init);
