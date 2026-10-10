#include "stm32f10x.h"
#include <stdio.h>
#include <math.h> 
#define M_PI 3.14159265358979323846f 


#define MPU9250_ADDR    0x68    
#define G_SENSITIVITY   131.0f  
#define DT              0.01f   

float pitch = 0.0f, roll = 0.0f;

void delay_ms(int n);
void USART1_Config(void);
void I2C1_Init(void);
void MPU9250_Init(void);
void MPU9250_Write_Byte(u8 reg, u8 data);
u8 MPU9250_Read_Byte(u8 reg);

void MPU9250_Read_6Axis_Data(short *ax, short *ay, short *az, short *gx, short *gy, short *gz);
int fputc(int ch, FILE *f);

int main(void)
{
    short ax, ay, az, gx, gy, gz; 
    float accel_pitch, accel_roll;    
    float gyro_pitch_rate, gyro_roll_rate; 

    USART1_Config();
    I2C1_Init();    
    MPU9250_Init(); 
    
    printf("--- Complementary Filter Demo ---\r\n");
    delay_ms(100);

    while(1)
    {
        MPU9250_Read_6Axis_Data(&ax, &ay, &az, &gx, &gy, &gz);
        
        accel_roll = atan2((float)ay, (float)az) * 180.0f / M_PI;
        accel_pitch = atan2((float)-ax, sqrt((float)ay * ay + (float)az * az)) * 180.0f / M_PI;
        
        gyro_roll_rate = (float)gx / G_SENSITIVITY;
        gyro_pitch_rate = (float)gy / G_SENSITIVITY;
        
        roll = 0.98f * (roll + gyro_roll_rate * DT) + 0.02f * accel_roll;
        pitch = 0.98f * (pitch + gyro_pitch_rate * DT) + 0.02f * accel_pitch;

        char buffer[64]; 
        sprintf(buffer, "P:%.2f,R:%.2f\n", pitch, roll); 
        
        for(int i = 0; buffer[i] != '\0'; i++)
        {
            fputc(buffer[i], NULL); 
        }
        
        delay_ms(10);
    }
}


void MPU9250_Init(void)
{
    MPU9250_Write_Byte(0x6B, 0x00); 
    MPU9250_Write_Byte(0x1A, 0x06); 
    MPU9250_Write_Byte(0x1B, 0x00);
    MPU9250_Write_Byte(0x1C, 0x00);
}


void MPU9250_Read_6Axis_Data(short *ax, short *ay, short *az, short *gx, short *gy, short *gz)
{
    u8 buf[14];
    buf[0] = MPU9250_Read_Byte(0x3B); buf[1] = MPU9250_Read_Byte(0x3C); // Accel X
    buf[2] = MPU9250_Read_Byte(0x3D); buf[3] = MPU9250_Read_Byte(0x3E); // Accel Y
    buf[4] = MPU9250_Read_Byte(0x3F); buf[5] = MPU9250_Read_Byte(0x40); // Accel Z
    
    // ?????? (0x41, 0x42)
    
    buf[6] = MPU9250_Read_Byte(0x43); buf[7] = MPU9250_Read_Byte(0x44); // Gyro X
    buf[8] = MPU9250_Read_Byte(0x45); buf[9] = MPU9250_Read_Byte(0x46); // Gyro Y
    buf[10]= MPU9250_Read_Byte(0x47); buf[11]= MPU9250_Read_Byte(0x48); // Gyro Z

    *ax = (buf[0] << 8) | buf[1];
    *ay = (buf[2] << 8) | buf[3];
    *az = (buf[4] << 8) | buf[5];
    
    *gx = (buf[6] << 8) | buf[7];
    *gy = (buf[8] << 8) | buf[9];
    *gz = (buf[10]<< 8) | buf[11];
}


void I2C1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    I2C_InitTypeDef  I2C_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    I2C_InitStructure.I2C_ClockSpeed = 100000; 
    I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_2;
    I2C_InitStructure.I2C_OwnAddress1 = 0x30;
    I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C1, &I2C_InitStructure);

    I2C_Cmd(I2C1, ENABLE);
}

void MPU9250_Write_Byte(u8 reg, u8 data)
{
    I2C_GenerateSTART(I2C1, ENABLE);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C1, MPU9250_ADDR << 1, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C1, reg);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_SendData(I2C1, data);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_GenerateSTOP(I2C1, ENABLE);
}


u8 MPU9250_Read_Byte(u8 reg)
{
    u8 data;

    while(I2C_GetFlagStatus(I2C1, I2C_FLAG_BUSY));

    I2C_GenerateSTART(I2C1, ENABLE);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C1, MPU9250_ADDR << 1, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));
    I2C_Cmd(I2C1, ENABLE); // SCL????
    I2C_SendData(I2C1, reg);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_GenerateSTART(I2C1, ENABLE);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C1, MPU9250_ADDR << 1, I2C_Direction_Receiver);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED));

    while(I2C_GetFlagStatus(I2C1, I2C_FLAG_RXNE) == RESET);
    data = I2C_ReceiveData(I2C1);

    I2C_AcknowledgeConfig(I2C1, DISABLE);
    I2C_GenerateSTOP(I2C1, ENABLE);
    
    return data;
}


void USART1_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = 9600;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStructure);

    USART_Cmd(USART1, ENABLE);
}


int fputc(int ch, FILE *f)
{
    USART_SendData(USART1, (uint8_t) ch);

    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    
    return ch;
}


void delay_ms(int n)
{
    volatile int i, j;
    for(i = 0; i < n; i++)
    {
        for(j = 0; j < 10000; j++); 
    }
}   
