/**
 ******************************************************************************
 * @file	uart1_task.c
 * @author	Hampus Sandberg
 * @version	0.1
 * @date	2014-09-06
 * @brief
 ******************************************************************************
	Copyright (c) 2014 Hampus Sandberg.

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation, either
	version 3 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "uart1_task.h"

#include "relay.h"
#include "spi_flash_memory_map.h"
#include "spi_flash.h"

#include <string.h>
#include <stdbool.h>

/* Private defines -----------------------------------------------------------*/
#define UART_CHANNEL	(USART1)

#define UART_TX_PIN		(GPIO_PIN_9)
#define UART_RX_PIN		(GPIO_PIN_10)
#define UART_PORT		(GPIOA)

#define RX_BUFFER_SIZE	(256)

/* Private typedefs ----------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
static RelayDevice switchRelay = {
		.gpioPort = GPIOC,
		.gpioPin = GPIO_PIN_0,
		.startState = RelayState_Off,
		.msBetweenStateChange = 500};
static RelayDevice powerRelay = {
		.gpioPort = GPIOE,
		.gpioPin = GPIO_PIN_0,
		.startState = RelayState_Off,
		.msBetweenStateChange = 500};

/* Default UART handle */
static UART_HandleTypeDef UART_Handle = {
		.Instance			= UART_CHANNEL,
		.Init.BaudRate		= UARTBaudRate_115200,
		.Init.WordLength 	= UART_WORDLENGTH_8B,
		.Init.StopBits		= UART_STOPBITS_1,
		.Init.Parity		= UART_PARITY_NONE,
		.Init.Mode			= UART_MODE_TX_RX,
		.Init.HwFlowCtl		= UART_HWCONTROL_NONE,
};

static UARTSettings prvCurrentSettings;
static SemaphoreHandle_t xSemaphore;

static uint8_t prvReceivedByte;
static uint8_t prvTxBuffer[128];
static uint32_t prvSizeOfDataInTxBuffer;

static uint8_t prvRxBuffer1[RX_BUFFER_SIZE];
static uint32_t prvRxBuffer1CurrentIndex = 0;
static uint32_t prvRxBuffer1Count = 0;
static BUFFERState prvRxBuffer1State = BUFFERState_Writing;
static TimerHandle_t prvBuffer1ClearTimer;

static uint8_t prvRxBuffer2[RX_BUFFER_SIZE];
static uint32_t prvRxBuffer2CurrentIndex = 0;
static uint32_t prvRxBuffer2Count = 0;
static BUFFERState prvRxBuffer2State = BUFFERState_Writing;
static TimerHandle_t prvBuffer2ClearTimer;

static uint32_t prvFlashWriteAddress = FLASH_ADR_UART1_DATA;

/* Private function prototypes -----------------------------------------------*/
static void prvHardwareInit();
static void prvEnableUart1Interface();
static void prvDisableUart1Interface();

static void prvBuffer1ClearTimerCallback();
static void prvBuffer2ClearTimerCallback();

/* Functions -----------------------------------------------------------------*/
/**
 * @brief	The main task for the UART1 channel
 * @param	pvParameters:
 * @retval	None
 */
void uart1Task(void *pvParameters)
{
	/* Mutex semaphore to manage when it's ok to send and receive new data */
	xSemaphore = xSemaphoreCreateMutex();

	prvBuffer1ClearTimer = xTimerCreate("Buf1Clear0", 10, pdFALSE, 0, prvBuffer1ClearTimerCallback);
	prvBuffer2ClearTimer = xTimerCreate("Buf2Clear1", 10, pdFALSE, 0, prvBuffer2ClearTimerCallback);

	prvHardwareInit();

	/* TODO: Read these from FLASH instead */
	prvCurrentSettings.baudRate = UART_Handle.Init.BaudRate;
	prvCurrentSettings.mode = UART_Handle.Init.Mode;
//	prvFlashWriteAddress =

	/* The parameter in vTaskDelayUntil is the absolute time
	 * in ticks at which you want to be woken calculated as
	 * an increment from the time you were last woken. */
	TickType_t xNextWakeTime;
	/* Initialize xNextWakeTime - this only needs to be done once. */
	xNextWakeTime = xTaskGetTickCount();

	vTaskDelay(1000 / portTICK_PERIOD_MS);
	SPI_FLASH_EraseSector(FLASH_ADR_UART1_DATA);

	uint8_t* data = "UART1 Debug! ";
	while (1)
	{
		vTaskDelayUntil(&xNextWakeTime, 500 / portTICK_PERIOD_MS);

		/* Transmit debug data if that mode is active */
		if (prvCurrentSettings.connection == UARTConnection_Connected && prvCurrentSettings.mode == UARTMode_DebugTX)
			uart1Transmit(data, strlen(data));
	}
}

/**
 * @brief	Set the power of the UART1
 * @param	Power: The power to set, UART1Power_3V3 or UART1Power_5V
 * @retval	SUCCES: Everything went ok
 * @retval	ERROR: Something went wrong
 */
ErrorStatus uart1SetPower(UARTPower Power)
{
	RelayStatus status = RelayStatus_NotEnoughTimePassed;;
	if (Power == UARTPower_3V3)
		status = RELAY_SetState(&powerRelay, RelayState_On);
	else if (Power == UARTPower_5V)
		status = RELAY_SetState(&powerRelay, RelayState_Off);

	if (status == RelayStatus_Ok)
	{
		prvCurrentSettings.power = Power;
		return SUCCESS;
	}
	else
		return ERROR;
}

/**
 * @brief	Set whether or not the output should be connected to the connector
 * @param	Connection: Can be any value of UART1Connection
 * @retval	SUCCES: Everything went ok
 * @retval	ERROR: Something went wrong
 */
ErrorStatus uart1SetConnection(UARTConnection Connection)
{
	RelayStatus status = RelayStatus_NotEnoughTimePassed;
	if (Connection == UARTConnection_Connected)
		status = RELAY_SetState(&switchRelay, RelayState_On);
	else if (Connection == UARTConnection_Disconnected)
		status = RELAY_SetState(&switchRelay, RelayState_Off);

	if (status == RelayStatus_Ok)
	{
		prvCurrentSettings.connection = Connection;
		if (Connection == UARTConnection_Connected)
			prvEnableUart1Interface();
		else
			prvDisableUart1Interface();
		return SUCCESS;
	}
	else
		return ERROR;
}

/**
 * @brief	Get the current settings of the UART1 channel
 * @param	None
 * @retval	A UART1Settings with all the settings
 */
UARTSettings uart1GetSettings()
{
	return prvCurrentSettings;
}

/**
 * @brief	Set the settings of the UART1 channel
 * @param	Settings: New settings to use
 * @retval	SUCCESS: Everything went ok
 * @retval	ERROR: Something went wrong
 */
ErrorStatus uart1SetSettings(UARTSettings* Settings)
{
	mempcpy(&prvCurrentSettings, Settings, sizeof(UARTSettings));

	/* Set the values in the USART handle */
	UART_Handle.Init.BaudRate = prvCurrentSettings.baudRate;
	if (prvCurrentSettings.mode == UARTMode_DebugTX)
		UART_Handle.Init.Mode = UARTMode_TX_RX;
	else
		UART_Handle.Init.Mode = prvCurrentSettings.mode;

	return SUCCESS;
}

/**
 * @brief	Returns the address which the UART1 data will be written to next
 * @param	None
 * @retval	The address
 */
uint32_t uart1GetCurrentWriteAddress()
{
	return prvFlashWriteAddress;
}

/**
 * @brief	Transmit data
 * @param	Data: Pointer to the buffer to send
 * @param	Size: Size of the buffer
 * @retval	None
 */
void uart1Transmit(uint8_t* Data, uint32_t Size)
{
	/* Make sure the uart is available */
	if (Size != 0 && xSemaphoreTake(xSemaphore, 100) == pdTRUE)
	{
		/* Copy data to the internal buffer and start sending */
		memcpy(prvTxBuffer, Data, Size);
		prvSizeOfDataInTxBuffer = Size;

		HAL_UART_Transmit_IT(&UART_Handle, prvTxBuffer, prvSizeOfDataInTxBuffer);
	}
}

/* Private functions .--------------------------------------------------------*/
/**
 * @brief	Initializes the hardware
 * @param	None
 * @retval	None
 */
static void prvHardwareInit()
{
	/* Init relays */
	RELAY_Init(&switchRelay);
	RELAY_Init(&powerRelay);

	/* Init GPIO */
	__GPIOA_CLK_ENABLE();

	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.Pin  		= UART_TX_PIN | UART_RX_PIN;
	GPIO_InitStructure.Mode  		= GPIO_MODE_AF_PP;
	GPIO_InitStructure.Alternate 	= GPIO_AF7_USART1;
	GPIO_InitStructure.Pull			= GPIO_NOPULL;
	GPIO_InitStructure.Speed 		= GPIO_SPEED_HIGH;
	HAL_GPIO_Init(UART_PORT, &GPIO_InitStructure);
}

/**
 * @brief	Enables the UART1 interface with the current settings
 * @param	None
 * @retval	None
 */
static void prvEnableUart1Interface()
{
	/* Enable UART clock */
	__USART1_CLK_ENABLE();

	/* Configure priority and enable interrupt */
	HAL_NVIC_SetPriority(USART1_IRQn, configLIBRARY_LOWEST_INTERRUPT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);

	/* Enable the UART */
	HAL_UART_Init(&UART_Handle);

	/* If we are in RX mode we should start receiving data */
	if (UART_Handle.Init.Mode == UARTMode_RX || UART_Handle.Init.Mode == UARTMode_TX_RX)
		HAL_UART_Receive_IT(&UART_Handle, &prvReceivedByte, 1);
}

/**
 * @brief	Disables the UART1 interface
 * @param	None
 * @retval	None
 */
static void prvDisableUart1Interface()
{
	HAL_NVIC_DisableIRQ(USART1_IRQn);
	HAL_UART_DeInit(&UART_Handle);
	__USART1_CLK_DISABLE();
	xSemaphoreGive(xSemaphore);

	prvRxBuffer1CurrentIndex = 0;
	prvRxBuffer1Count = 0;
	prvRxBuffer1State = BUFFERState_Writing;
	prvRxBuffer2CurrentIndex = 0;
	prvRxBuffer2Count = 0;
	prvRxBuffer2State = BUFFERState_Writing;
}

static void prvBuffer1ClearTimerCallback()
{
	/* Set the buffer to reading state */
	prvRxBuffer1State = BUFFERState_Reading;

	/* Write the data to FLASH */
	for (uint32_t i = 0; i < prvRxBuffer1Count; i++)
		SPI_FLASH_WriteByte(prvFlashWriteAddress++, prvRxBuffer1[i]);
	/* TODO: Something strange with the FLASH page write so doing one byte at a time now */
//	/* Write all the data in the buffer to SPI FLASH */
//	SPI_FLASH_WriteBuffer(prvRxBuffer1, prvFlashWriteAddress, prvRxBuffer1Count);
//	/* Update the write address */
//	prvFlashWriteAddress += prvRxBuffer1Count;

	/* Reset the buffer */
	prvRxBuffer1CurrentIndex = 0;
	prvRxBuffer1Count = 0;
	prvRxBuffer1State = BUFFERState_Writing;
}

static void prvBuffer2ClearTimerCallback()
{
	/* Set the buffer to reading state */
	prvRxBuffer2State = BUFFERState_Reading;

	/* Write the data to FLASH */
	for (uint32_t i = 0; i < prvRxBuffer2Count; i++)
		SPI_FLASH_WriteByte(prvFlashWriteAddress++, prvRxBuffer2[i]);
	/* TODO: Something strange with the FLASH page write so doing one byte at a time now */
//	/* Write all the data in the buffer to SPI FLASH */
//	SPI_FLASH_WriteBuffer(prvRxBuffer2, prvFlashWriteAddress, prvRxBuffer2Count);
//	/* Update the write address */
//	prvFlashWriteAddress += prvRxBuffer2Count;

	/* Reset the buffer */
	prvRxBuffer2CurrentIndex = 0;
	prvRxBuffer2Count = 0;
	prvRxBuffer2State = BUFFERState_Writing;
}

/* Interrupt Handlers --------------------------------------------------------*/
/**
  * @brief  This function handles UART1 interrupt request
  * @param  None
  * @retval None
  */
void USART1_IRQHandler(void)
{
	HAL_UART_IRQHandler(&UART_Handle);
}

/* HAL Callback functions ----------------------------------------------------*/
/**
  * @brief  Tx Transfer completed callback
  * @param  None
  * @retval None
  */
void uart1TxCpltCallback()
{
	/* Give back the semaphore now that we are done */
	xSemaphoreGiveFromISR(xSemaphore, NULL);
}

/**
  * @brief  Rx Transfer completed callback
  * @param  None
  * @retval None
  */
void uart1RxCpltCallback()
{
	if (prvRxBuffer1State != BUFFERState_Reading && prvRxBuffer1Count < RX_BUFFER_SIZE)
	{
		prvRxBuffer1State = BUFFERState_Writing;
		prvRxBuffer1[prvRxBuffer1CurrentIndex++] = prvReceivedByte;
		prvRxBuffer1Count++;
		/* Start the timer which will clear the buffer if it's not already started */
		if (xTimerIsTimerActive(prvBuffer1ClearTimer) == pdFALSE)
			xTimerStartFromISR(prvBuffer1ClearTimer, NULL);
	}
	else if (prvRxBuffer2State != BUFFERState_Reading && prvRxBuffer2Count < RX_BUFFER_SIZE)
	{
		prvRxBuffer2State = BUFFERState_Writing;
		prvRxBuffer2[prvRxBuffer2CurrentIndex++] = prvReceivedByte;
		prvRxBuffer2Count++;
		/* Start the timer which will clear the buffer if it's not already started */
		if (xTimerIsTimerActive(prvBuffer2ClearTimer) == pdFALSE)
			xTimerStartFromISR(prvBuffer2ClearTimer, NULL);
	}
	else
	{
		/* No buffer available, something has gone wrong */
		HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_3);
	}

	/* Continue receiving data */
	HAL_UART_Receive_IT(&UART_Handle, &prvReceivedByte, 1);
	/* Give back the semaphore now that we are done */
	xSemaphoreGiveFromISR(xSemaphore, NULL);
}

/**
  * @brief  UART error callback
  * @param  None
  * @retval None
  */
 void uart1ErrorCallback()
{
	/* Give back the semaphore now that we are done */
	xSemaphoreGiveFromISR(xSemaphore, NULL);
	/* TODO: Indicate error somehow ??? */
}
