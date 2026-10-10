# 当前系统设计

```text
MPU9250 加速度计 + 陀螺仪
          │ I2C1 (PB6/PB7)
          ▼
STM32F103C8T6 固件
  ├─ 原始 6 轴数据读取
  ├─ 加速度计角度估计
  ├─ 陀螺仪积分
  └─ 互补滤波，输出 Pitch/Roll
          │ USART1 (PA9/PA10), 9600 baud
          ▼
CH340 / USB-UART
          │ COM 口
          ▼
Python pyserial 接收
          │ 行协议：P:<pitch>,R:<roll>
          ▼
VPython 三维模型姿态更新
```

## 设计边界

PC 上位机不依赖直接访问 STM32 寄存器；它只消费串口协议。解析器独立于 VPython，因而可单独做单元测试。硬件固件与 PC 程序通过明确的文本协议耦合，后续可逐步加入序号、设备时间戳、原始传感器数据与错误状态，但升级协议时必须同步更新 MCU、PC 端和测试。
