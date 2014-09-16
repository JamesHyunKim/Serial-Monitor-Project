/**
 ******************************************************************************
 * @file	can1_task.c
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
#include "can1_task.h"

#include "relay.h"

#include <string.h>

/* Private defines -----------------------------------------------------------*/
#define CANx						CAN1
#define CANx_CLK_ENABLE()			__CAN1_CLK_ENABLE()
#define CANx_GPIO_CLK_ENABLE()		__GPIOB_CLK_ENABLE()

#define CANx_FORCE_RESET()			__CAN1_FORCE_RESET()
#define CANx_RELEASE_RESET()		__CAN1_RELEASE_RESET()

#define CANx_TX_PIN					GPIO_PIN_9
#define CANx_TX_GPIO_PORT			GPIOB
#define CANx_TX_AF					GPIO_AF9_CAN1
#define CANx_RX_PIN					GPIO_PIN_8
#define CANx_RX_GPIO_PORT			GPIOB
#define CANx_RX_AF					GPIO_AF9_CAN1

#define CANx_RX_IRQn				CAN1_RX0_IRQn
#define CANx_RX_IRQHandler			CAN1_RX0_IRQHandler

/* Private typedefs ----------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
static RelayDevice switchRelay = {
		.gpioPort = GPIOE,
		.gpioPin = GPIO_PIN_2,
		.startState = RelayState_Off,
		.msBetweenStateChange = 1000
};
static RelayDevice terminationRelay = {
		.gpioPort = GPIOE,
		.gpioPin = GPIO_PIN_3,
		.startState = RelayState_Off,
		.msBetweenStateChange = 1000
};

static CanTxMsgTypeDef TxMessage;
static CanRxMsgTypeDef RxMessage;

static CAN_HandleTypeDef CAN_Handle = {
		.Instance			= CANx,
		.pTxMsg 			= &TxMessage,
		.pRxMsg 			= &RxMessage,
		.Init.Prescaler 	= CANPrescaler_125k,
		.Init.Mode 			= CAN_MODE_NORMAL,
		.Init.SJW 			= CANSJW_125k,
		.Init.BS1 			= CANBS1_125k,
		.Init.BS2 			= CANBS2_125k,
		.Init.TTCM 			= DISABLE,
		.Init.ABOM 			= DISABLE,
		.Init.AWUM 			= DISABLE,
		.Init.NART 			= DISABLE,
		.Init.RFLM 			= DISABLE,
		.Init.TXFP 			= DISABLE,
};

static CAN_FilterConfTypeDef CAN_Filter = {
		.FilterIdHigh 			= 0x0000,
		.FilterIdLow 			= 0x0000,
		.FilterMaskIdHigh 		= 0x0000,
		.FilterMaskIdLow		= 0x0000,
		.FilterFIFOAssignment 	= 0,
		.FilterNumber 			= 0,
		.FilterMode 			= CAN_FILTERMODE_IDMASK,
		.FilterScale 			= CAN_FILTERSCALE_32BIT,
		.FilterActivation 		= ENABLE,
		.BankNumber 			= 14,	/* This defines the start bank for the CAN2 interface (Slave) in the range 0 to 27. */
};

static CANSettings prvCurrentSettings = {
		.connection 			= CANConnection_Disconnected,
		.termination			= CANTermination_Disconnected,
};

static uint8_t* prvCanStatusMessages[4] = {
		"CAN1: HAL_OK",
		"CAN1: HAL_ERROR",
		"CAN1: HAL_BUSY",
		"CAN1: HAL_TIMEOUT",
};

/* Private function prototypes -----------------------------------------------*/
static void prvHardwareInit();
static ErrorStatus prvEnableCan1Interface();
static ErrorStatus prvDisableCan1Interface();

/* Functions -----------------------------------------------------------------*/
/**
 * @brief	The main task for the CAN1 channel
 * @param	pvParameters:
 * @retval	None
 */
void can1Task(void *pvParameters)
{
	prvHardwareInit();

	/* The parameter in vTaskDelayUntil is the absolute time
	 * in ticks at which you want to be woken calculated as
	 * an increment from the time you were last woken. */
	TickType_t xNextWakeTime;
	/* Initialize xNextWakeTime - this only needs to be done once. */
	xNextWakeTime = xTaskGetTickCount();

	while (1)
	{
		vTaskDelayUntil(&xNextWakeTime, 1000 / portTICK_PERIOD_MS);
		/* Transmit debug data */
		if (prvCurrentSettings.connection == CANConnection_Connected)
		{
			/* Set the data to be transmitted */
			CAN_Handle.pTxMsg->Data[0] = 0xAA;
			CAN_Handle.pTxMsg->Data[1] = 0xAA;

			/* Start the Transmission process */
			HAL_StatusTypeDef status = HAL_CAN_Transmit(&CAN_Handle, 50);

#if 1
			MESSAGES_SendDebugMessage(prvCanStatusMessages[status]);
#endif
		}
	}
}

/**
 * @brief	Set whether or not to use termination on the output
 * @param	Termination: Can be any value of CANTermination
 * @retval	SUCCES: Everything went ok
 * @retval	ERROR: Something went wrong
 */
ErrorStatus can1SetTermination(CANTermination Termination)
{
	RelayStatus status;
	if (Termination == CANTermination_Connected)
		status = RELAY_SetState(&terminationRelay, RelayState_On);
	else if (Termination == CANTermination_Disconnected)
		status = RELAY_SetState(&terminationRelay, RelayState_Off);

	if (status == RelayStatus_Ok)
		return SUCCESS;
	else
		return ERROR;
}

/**
 * @brief	Set whether or not the output should be connected to the connector
 * @param	Connection: Can be any value of CANConnection
 * @retval	SUCCES: Everything went ok
 * @retval	ERROR: Something went wrong
 */
ErrorStatus can1SetConnection(CANConnection Connection)
{
	RelayStatus relayStatus = RelayStatus_NotEnoughTimePassed;
	if (Connection == CANConnection_Connected)
		relayStatus = RELAY_SetState(&switchRelay, RelayState_On);
	else if (Connection == CANConnection_Disconnected)
		relayStatus = RELAY_SetState(&switchRelay, RelayState_Off);

	if (relayStatus == RelayStatus_Ok)
	{
		ErrorStatus errorStatus = ERROR;
		if (Connection == CANConnection_Connected)
			errorStatus = prvEnableCan1Interface();
		else
			errorStatus = prvDisableCan1Interface();

		if (errorStatus == SUCCESS)
		{
			prvCurrentSettings.connection = Connection;
			return SUCCESS;
		}
		else
		{
			RELAY_SetState(&switchRelay, RelayState_Off);
			goto error;
		}
	}

error:
	return ERROR;
}

/**
 * @brief	Get the current settings of the CAN1 channel
 * @param	None
 * @retval	A pointer to the current settings
 */
CANSettings* can1GetSettings()
{
	return &prvCurrentSettings;
}

/**
 * @brief	Set the settings of the CAN1 channel
 * @param	Settings: New settings to use
 * @retval	SUCCES: Everything went ok
 * @retval	ERROR: Something went wrong
 */
ErrorStatus can1SetSettings(CANSettings* Settings)
{
	mempcpy(&prvCurrentSettings, Settings, sizeof(CANSettings));

	return SUCCESS;
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
	RELAY_Init(&terminationRelay);

	/* Configure peripheral GPIO */
	CANx_GPIO_CLK_ENABLE();
	GPIO_InitTypeDef GPIO_InitStruct;

	/* CAN2 TX GPIO pin configuration */
	GPIO_InitStruct.Pin 		= CANx_TX_PIN;
	GPIO_InitStruct.Mode 		= GPIO_MODE_AF_PP;
	GPIO_InitStruct.Speed 		= GPIO_SPEED_FAST;
	GPIO_InitStruct.Pull 		= GPIO_PULLUP;
	GPIO_InitStruct.Alternate 	= CANx_TX_AF;
	HAL_GPIO_Init(CANx_TX_GPIO_PORT, &GPIO_InitStruct);

	/* CAN2 RX GPIO pin configuration */
	GPIO_InitStruct.Pin 		= CANx_RX_PIN;
	GPIO_InitStruct.Mode 		= GPIO_MODE_AF_PP;
	GPIO_InitStruct.Speed 		= GPIO_SPEED_FAST;
	GPIO_InitStruct.Pull		= GPIO_PULLUP;
	GPIO_InitStruct.Alternate 	= CANx_RX_AF;
	HAL_GPIO_Init(CANx_RX_GPIO_PORT, &GPIO_InitStruct);
}

/**
 * @brief	Enables the CAN1 interface with the current settings
 * @param	None
 * @retval	None
 */
static ErrorStatus prvEnableCan1Interface()
{
	/*##-1- Enable peripheral Clocks ###########################################*/
	/* CAN1 Peripheral clock enable */
	CANx_CLK_ENABLE();

	/*##-2- Configure the NVIC #################################################*/
	/* NVIC configuration for CAN1 Reception complete interrupt */
	HAL_NVIC_SetPriority(CANx_RX_IRQn, configLIBRARY_LOWEST_INTERRUPT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(CANx_RX_IRQn);

	/*##-3- Configure the CAN peripheral #######################################*/
	if (HAL_CAN_Init(&CAN_Handle) != HAL_OK)
	{
		/* Initialization Error */
		goto error;
	}

	/*##-4- Configure the CAN Filter ###########################################*/
	if (HAL_CAN_ConfigFilter(&CAN_Handle, &CAN_Filter) != HAL_OK)
	{
		/* Filter configuration Error */
		goto error;
	}

	/*##-5- Configure Transmission process #####################################*/
	CAN_Handle.pTxMsg->StdId = 0x321;
	CAN_Handle.pTxMsg->ExtId = 0x01;
	CAN_Handle.pTxMsg->RTR = CAN_RTR_DATA;
	CAN_Handle.pTxMsg->IDE = CAN_ID_STD;
	CAN_Handle.pTxMsg->DLC = 2;

	/*##-6- Start the Reception process and enable reception interrupt #########*/
	if (HAL_CAN_Receive_IT(&CAN_Handle, CAN_FIFO0) != HAL_OK)
	{
		/* Reception Error */
		goto error;
	}

	return SUCCESS;

error:
	/* Something went wrong so disable */
	prvDisableCan1Interface();
	return ERROR;
}

/**
 * @brief	Disables the CAN1 interface
 * @param	None
 * @retval	None
 */
static ErrorStatus prvDisableCan1Interface()
{
	/*##-1- Reset peripherals ##################################################*/
	CANx_FORCE_RESET();
	CANx_RELEASE_RESET();

	/*##-2- Disable the NVIC for CAN reception #################################*/
	HAL_NVIC_DisableIRQ(CANx_RX_IRQn);

	return SUCCESS;
}

/* Interrupt Handlers --------------------------------------------------------*/
/**
* @brief  This function handles CAN1 RX0 interrupt request.
* @param  None
* @retval None
*/
void CAN1_RX0_IRQHandler(void)
{
	HAL_CAN_IRQHandler(&CAN_Handle);
}

/**
* @brief  This function handles CAN1 RX1 interrupt request.
* @param  None
* @retval None
*/
void CAN1_RX1_IRQHandler(void)
{
	HAL_CAN_IRQHandler(&CAN_Handle);
}

/**
* @brief  This function handles CAN1 TX interrupt request.
* @param  None
* @retval None
*/
void CAN1_TX_IRQHandler(void)
{
	HAL_CAN_IRQHandler(&CAN_Handle);
}

/* HAL Callback functions ----------------------------------------------------*/
/**
  * @brief  Rx Transfer completed callback
  * @param  None
  * @retval None
  */
void can1RxCpltCallback()
{
	if ((CAN_Handle.pRxMsg->StdId == 0x321) && (CAN_Handle.pRxMsg->IDE == CAN_ID_STD) && (CAN_Handle.pRxMsg->DLC == 2))
	{
//		LED_Display(CAN_Handle.pRxMsg->Data[0]);
		uint8_t ubKeyNumber = CAN_Handle.pRxMsg->Data[0];
	}

	/* Receive */
	if (HAL_CAN_Receive_IT(&CAN_Handle, CAN_FIFO0) != HAL_OK)
	{
		/* Reception Error */
//		Error_Handler();
	}
}
