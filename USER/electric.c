#include "stm32f4xx_gpio.h"
#include "stm32f4xx_usart.h"
#include "misc.h"
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "electric.h"
#include "uart_debug.h"
#include "gateway_protocol.h"
#include "ballast_protocol.h"

#define ELECTRIC_TASK_STACK_SIZE		(configMINIMAL_STACK_SIZE + 1024*5)
#define ELECTRIC_BUFF_SIZE  100

xSemaphoreHandle EleTx_semaphore;
static xQueueHandle ElectricQueue;

typedef struct 
{
	u8 index;
	u8 length;
  u8 Buff[100];
}ElectricMessage;

const static MessageHandlerMap Electric_MessageMaps[] =  //二位数组的初始化
{
	{GATEPARAM,      HandleGatewayParam},     /*0x01; 网关参数下载*/           
//	{LIGHTPARAM,     HandleLightParam},       /*0x02; 灯参数下载*/              
//	{DIMMING,        HandleLightDimmer},      /*0x04; 灯调光控制*/
//	{LAMPSWITCH,     HandleLightOnOff},       /*0x05; 灯开关控制*/
//	{READDATA,       HandleReadBSNData},      /*0x06; 读镇流器数据*/
//	{DATAQUERY,      HandleGWDataQuery},      /*0x08; 网关数据查询*/           		    
//	{VERSIONQUERY,   HandleGWVersQuery},      /*0x0C; 查网关软件版本号*/      
//	{SETPARAMLIMIT,  HandleSetParamDog},      /*0x21; 设置光强度区域和时间域划分点参数*/
//	{STRATEGYDOWN,   HandleStrategy},         /*0x22; 策略下载*/
//	{GATEUPGRADE,    HandleGWUpgrade},        /*0x37; 网关远程升级*/
//	{TIMEADJUST,     HandleAdjustTime},       /*0x42; 校时*/                     
//	{LUXVALUE,       HandleLuxGather},        /*0x43; 接收到光照度强度值*/		
//	{RESTART,        HandleRestart},          /*0x3F; 设备复位*/               
};


ElectricMessage EleTxData;
ElectricMessage EleRxData;

static void ElectricUartInit(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	
	/*****************使能IO口和串口时钟*******************/
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB,ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);
 
	/*****************串口对应引脚复用映射****************/
	GPIO_PinAFConfig(GPIOB,GPIO_PinSource6,GPIO_AF_USART1);
	GPIO_PinAFConfig(GPIOB,GPIO_PinSource7,GPIO_AF_USART1);
	
	/********************USART端口配置********************/
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7; 
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;	
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP; 
	GPIO_Init(GPIOB,&GPIO_InitStructure); 

   /*****************USART初始化设置********************/
	USART_InitStructure.USART_BaudRate = 9600;//波特率设置
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;//字长为8位数据格式
	USART_InitStructure.USART_StopBits = USART_StopBits_1;//一个停止位
	USART_InitStructure.USART_Parity = USART_Parity_No;//无奇偶校验位
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;//无硬件数据流控制
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;	//收发模式
  USART_Init(USART1, &USART_InitStructure); //初始化串口
	
  USART_Cmd(USART1, ENABLE);  //使能串口
	
	//USART_ClearFlag(USART1, USART_FLAG_TC);
	
	USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);//开启相关中断

	/*****************Usart1 NVIC配置***********************/
  NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;//串口中断通道
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=15;//抢占优先级3
	NVIC_InitStructure.NVIC_IRQChannelSubPriority =0;		//子优先级3
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;			//IRQ通道使能
	NVIC_Init(&NVIC_InitStructure);	//根据指定的参数初始化VIC寄存器�	
}

static void Electric_TX_DMA_Init(void)
{
  DMA_InitTypeDef  DMA_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2,ENABLE);//DMA1时钟使能 
  DMA_DeInit(DMA2_Stream7);//DMA1数据流4
	
	while (DMA_GetCmdStatus(DMA2_Stream7) != DISABLE){}//等待DMA可配置 
	
  /* 配置 DMA Stream */
  DMA_InitStructure.DMA_Channel = DMA_Channel_4;  //通道选择
  DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&USART1->DR;//DMA外设地址
  DMA_InitStructure.DMA_Memory0BaseAddr = (u32)&EleTxData.Buff;//DMA 存储器0地址
  DMA_InitStructure.DMA_DIR = DMA_DIR_MemoryToPeripheral;//存储器到外设模式
  DMA_InitStructure.DMA_BufferSize = 0x00;//数据传输量
  DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;//外设非增量模式
  DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;//存储器增量模式
  DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;//外设数据长度:8位
  DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;//存储器数据长度:8位
  DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;// 使用普通模式 
  DMA_InitStructure.DMA_Priority = DMA_Priority_Medium;//中等优先级
  DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Disable; //指定使用FIFO模式还是直接模式        
  DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;//制定了FIFO阈值
  DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_Single;//存储器突发单次传输
  DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;//外设突发单次传输
  DMA_Init(DMA2_Stream7, &DMA_InitStructure);//初始化DMA Stream
		
  DMA_ITConfig(DMA2_Stream7, DMA_IT_TC, ENABLE);
	USART_DMACmd(USART1,USART_DMAReq_Tx,ENABLE);  //使能串口3的DMA发送
		
	NVIC_InitStructure.NVIC_IRQChannel = DMA2_Stream7_IRQn;//串口中断通道
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=15;//抢占优先级
	NVIC_InitStructure.NVIC_IRQChannelSubPriority =0;		//子优先级
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;			//IRQ通道使能
	NVIC_Init(&NVIC_InitStructure);	//根据指定的参数初始化VIC寄存器�
}

static void EleRxTxDataInit(void)
{
  EleRxData.index = 0;
	EleRxData.length = 0;
	memset(EleRxData.Buff, 0, ELECTRIC_BUFF_SIZE);//Buff赋值为0

  EleTxData.index = 0;
	EleTxData.length = 0;
	memset(EleTxData.Buff, 0, ELECTRIC_BUFF_SIZE);//Buff赋值为0
}

static inline void ElectricRxDataInput(ElectricMessage *temp, char dat)
{
  if(temp->length < ELECTRIC_BUFF_SIZE)
  {
    temp->Buff[temp->index] = dat;
    temp->index++;
    temp->length++;
  }
}

static inline void ElectricRxDataClear(ElectricMessage *temp)
{
  memset(temp->Buff, 0, ELECTRIC_BUFF_SIZE);
	temp->index = 0;
	temp->length = 0;
}

void USART1_IRQHandler(void)
{
	unsigned char data;
	portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;
	
	if(USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
	{
    data = USART_ReceiveData(USART1);
	  if(data == 0x02)
		{
			ElectricRxDataClear(&EleRxData);
		}
			
		ElectricRxDataInput(&EleRxData,data);
			
		if(data == 0x03)
		{
			BCC_CheckSum(EleRxData.Buff,EleRxData.length-3);
			xQueueSendFromISR(ElectricQueue, &EleRxData.Buff, &xHigherPriorityTaskWoken);
			portEND_SWITCHING_ISR( xHigherPriorityTaskWoken );
			ElectricRxDataClear(&EleRxData);
		}
	}
}

void DMA2_Stream7_IRQHandler(void)//GSM_DMA发送中断
{
	DMA_ClearFlag(DMA2_Stream7, DMA_FLAG_TCIF7); 
	DMA_Cmd(DMA2_Stream7, DISABLE);
	xSemaphoreGive(EleTx_semaphore);
}


void EleDMA_TxBuff(char *buf, u8 buf_size)
{
	if(xSemaphoreTake(EleTx_semaphore, configTICK_RATE_HZ * 5) == pdTRUE) 
  {
		memcpy(EleTxData.Buff, buf, buf_size);
		EleTxData.length = buf_size;
		
		DMA_Cmd(DMA2_Stream7, DISABLE);                         //关闭DMA传输 
		while(DMA_GetCmdStatus(DMA2_Stream7) != DISABLE){}	    //确保DMA可以被设置  
		DMA_SetCurrDataCounter(DMA2_Stream7,EleTxData.length);  //数据传输量  
		DMA_Cmd(DMA2_Stream7, ENABLE);                          //开启DMA传输 
  }
}


static void ElectrolHardwareInit(void)
{
	ElectricUartInit();
	Electric_TX_DMA_Init();
}

static void vElectTask(void *parameter)
{
	u8 message[sizeof(EleRxData.Buff)];
	u8 protocol_type;
	
	while(1)
	{
		if(xQueueReceive(ElectricQueue, &message, configTICK_RATE_HZ / 10) == pdTRUE)
		{
			protocol_type = (chr2hex(message[5])<<4 | chr2hex(message[6]));
			const MessageHandlerMap *map = Electric_MessageMaps;
			for(; map->type != PROTOCOL_NULL; ++map)
			{
				if (protocol_type == map->type) 
				{
					map->handlerFunc(message);
					break;
				}
			}
		}
	}
}


void ElectricInit(void)
{
  ElectrolHardwareInit();
	EleRxTxDataInit();
	vSemaphoreCreateBinary(EleTx_semaphore);
  ElectricQueue = xQueueCreate(8, sizeof(ElectricMessage));	
	xTaskCreate(vElectTask, "ElectTask", ELECTRIC_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 4, NULL);
}
