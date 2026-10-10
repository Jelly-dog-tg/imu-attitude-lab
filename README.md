# IMU Attitude Lab：STM32 姿态测量与 PC 端三维可视化

这是一个真实硬件驱动的学习与作品集项目，核心链路是 **STM32F103C8T6 + MPU9250 → I2C 采集 → 互补滤波 → UART/CH340 → Python/VPython 实时 3D 姿态显示**。

> 当前已完成范围：真实硬件连接、6 轴（加速度计 + 陀螺仪）数据读取、互补滤波 Pitch/Roll 输出和上位机三维可视化。当前固件没有实现磁力计读取 / Yaw 融合，也没有实现可靠的位移定位。文档会明确区分已完成和计划扩展内容。

## 功能

- [x] STM32F103C8T6 Keil MDK 工程
- [x] I2C1 读取 MPU9250 加速度计、陀螺仪数据
- [x] 互补滤波输出 Pitch / Roll
- [x] USART1 文本数据输出（9600 baud）
- [x] Python 串口接收和 VPython 3D 模型显示
- [x] 真实硬件串口协议解析器及单元测试
- [x] Git 忽略规则，避免提交虚拟环境和 Keil 编译产物
- [ ] 实验数据保存 / 回放、异常帧与丢帧统计
- [ ] I2C 故障恢复、采样时钟改进与算法误差评估
- [ ] 磁力计校准与 Yaw 解算

## 运行上位机

需要 Python 3.10+、串口转 USB 设备和已在工作的 STM32 固件。

```powershell
python -m pip install -r requirements.txt
python -m unittest discover -s tests -v
python -m host.visualize --port COM3 --baud 9600
```

将 `COM3` 换成 Windows 设备管理器中 CH340 对应的串口号。波特率须与固件中的 `USART_BaudRate = 9600` 一致。启动前，关闭 XCOM 或其他已经打开同一个 COM 口的串口软件。

如果想保留你原来的 VPython 脚本，可对照 `docs/visualize_original.py.txt`；新的 `host/visualize.py` 做了协议解析拆分、命令行端口配置和异常行计数。

## 跑协议测试

```powershell
python -m unittest discover -s tests -v
```

这些自动化测试只测试 Python 协议解析与模拟器逻辑；它们不能代替硬件实测，也不能证明固件在所有传感器/接线条件下都稳定。

## 硬件与协议

- 接线：[`docs/HARDWARE_WIRING.md`](docs/HARDWARE_WIRING.md)
- 系统设计：[`docs/DESIGN.md`](docs/DESIGN.md)
- 完成情况及限制：[`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md)
- 迭代路线：[`docs/ROADMAP.md`](docs/ROADMAP.md)
- 固件说明：[`firmware/stm32f103/README.md`](firmware/stm32f103/README.md)

当前 MCU 输出格式是一行一帧：

```text
P:10.25,R:-5.50
```

其中 P 为俯仰角，R 为滚转角，单位为度。协议解析器也接受实验性 `X`、`Y` 扩展字段，但现有固件默认不输出位置数据；仅靠 MEMS 加速度双重积分得到的位置会快速漂移，不应作为可靠定位结果。

## 仓库目录

```text
imu-attitude-lab/
├── firmware/stm32f103/       # 现有 Keil 工程与固件说明
├── host/imu_lab/              # 协议、模拟器和命令行工具
├── host/visualize.py          # 对接真实 STM32 串口的 VPython 上位机
├── tests/                     # 单元测试
├── data/                      # 本地实验数据目录，不提交生成数据
├── docs/                      # 接线、设计、状态与迭代文档
└── requirements.txt
```

## 简历描述建议

可以根据实际复现情况写为：

> 基于 STM32F103C8T6 与 MPU9250 实现嵌入式姿态测量原型，完成 I2C 六轴传感器数据采集、加速度计/陀螺仪互补滤波及 Pitch/Roll 实时解算；通过 UART/CH340 建立上下位机通信，使用 Python、pyserial 和 VPython 开发实时三维姿态可视化工具，并补充串口协议解析测试与工程文档。

只能保留自己实际做过且能现场解释、复现的内容。不要将计划中的磁力计融合、Yaw、精度指标或可靠定位写成已经完成。
