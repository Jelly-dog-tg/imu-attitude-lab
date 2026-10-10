# IMU Attitude Lab：嵌入式姿态测量与上位机工具链

一个面向学习与作品集的端到端工程：以 STM32F103 + MPU9250 为目标硬件，逐步实现 IMU 数据采集、串口协议、姿态解算、PC 端可视化、数据记录和误差分析。

> 当前仓库是第一阶段起步版：先在没有硬件的电脑上运行“模拟数据 → 协议解析 → 文件记录 → 单元测试”流程。它不是已经验证过的 STM32 固件；真实硬件部分会在后续阶段接入并测试。

## 项目能力路线

- [x] 结构化的 Python 包、串口行协议解析器、可重复的模拟数据流
- [x] 协议解析单元测试
- [ ] PC 串口接收、实时曲线与 CSV 记录
- [ ] STM32F103 + MPU9250 传感器驱动与原始数据采集
- [ ] 加速度计/陀螺仪互补滤波与姿态角计算
- [ ] 磁力计校准、航向角与误差分析
- [ ] 故障注入、日志、配置、测试和持续集成
- [ ] 演示视频、架构图、复现实验步骤和发布版本

## 环境

- Windows 10/11 或 Linux
- Python 3.10+（起步版只用 Python 标准库）
- Git；Windows 用户可以继续使用 TortoiseGit
- 后续硬件阶段需要 STM32CubeIDE / STM32CubeMX，以及 STM32F103、MPU9250 模块和串口连接

## 第一次运行

在仓库根目录打开 PowerShell / Terminal：

```powershell
python --version
python -m unittest discover -s tests -v
python -m host.imu_lab.cli simulate --seconds 5 --hz 20 --output data/demo_stream.txt
```

成功后应看到测试通过提示，以及约 100 条模拟采样记录。`data/demo_stream.txt` 是本地生成文件，默认不会被 Git 跟踪。

## 串口协议（当前版本）

每行一条记录，逗号分隔，结尾为换行符：

```text
IMU,seq,t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,mx_uT,my_uT,mz_uT,roll_deg,pitch_deg,yaw_deg
```

- `seq`：递增采样序号
- `t_ms`：设备启动后的毫秒数
- `ax_g ... az_g`：加速度计，单位 g
- `gx_dps ... gz_dps`：陀螺仪，单位 °/s
- `mx_uT ... mz_uT`：磁力计，单位 μT
- `roll_deg / pitch_deg / yaw_deg`：姿态角，单位 °

当前模拟器输出的是合成数据，目的是验证数据管道，不代表真实传感器测量精度。

## 仓库结构

```text
imu-attitude-lab/
├── README.md
├── docs/
│   ├── DESIGN.md
│   └── ROADMAP.md
├── firmware/stm32f103/README.md
├── host/imu_lab/
│   ├── protocol.py
│   ├── simulator.py
│   └── cli.py
├── tests/test_protocol.py
├── data/.gitkeep
└── .gitignore
```

## 项目展示时应该诚实区分

- 只运行模拟器：软件起步版 / 数据协议原型。
- 接入真实 STM32 与 MPU9250，并验证传感器读数：嵌入式软硬件项目。
- 完成滤波、校准和误差评估：有算法验证的嵌入式测量系统。

请只把自己实际完成、能解释并复现的部分写进简历。
