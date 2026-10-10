# STM32F103C8T6 + MPU9250 固件

此目录中的 `KeilProject` 来自当前项目的 Keil MDK 工程，保留了现有 `main.c` 和工程配置文件。

## 当前已实现（按现有源代码核对）

- STM32F103C8T6 的 USART1 串口初始化，PA9 为 TX、PA10 为 RX，9600 baud。
- I2C1 使用 PB6/PB7 与 MPU9250 通信。
- 读取三轴加速度计和三轴陀螺仪共 6 轴原始数据。
- 利用加速度计角度与陀螺仪角速度进行互补滤波，输出 Pitch 与 Roll。
- 通过串口输出 `P:<pitch>,R:<roll>` 一行文本，供 PC 端可视化。

## 打开工程

1. 安装 Keil MDK-ARM，并通过 Pack Installer 安装工程所需的 STM32F1xx 设备支持包和外设组件。
2. 打开 `KeilProject/MyFirstLED.uvprojx`。
3. Rebuild 后连接 ST-Link 下载；实际编译器版本和设备包版本以本机 Keil 配置为准。
4. CH340 与 STM32 USART1 交叉连接：CH340 TXD → PA10，CH340 RXD → PA9，GND 共地。
5. 上位机波特率必须与固件一致（当前 9600）。

## 重要限制

- 当前 `main.c` 只读取加速度计和陀螺仪；虽然 MPU9250 芯片含磁力计，当前这份代码并未读取磁力计，因此不能把当前版本描述为已实现磁力计融合或 Yaw 解算。
- `DT` 当前固定为 0.01 s，表示代码按 100 Hz 设计；主循环使用软件延时，实际采样周期尚未通过示波器/时间戳严格测量。
- I2C 等待循环尚缺少超时与故障恢复；这是后续工程化改进点。
- 不要提交 Keil 自动生成的 `Objects/`、`Listings/` 或本机用户设置文件。
