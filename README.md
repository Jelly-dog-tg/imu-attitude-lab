# IMU Attitude Lab：STM32 姿态测量、Python 上位机与 C++ 数据处理核心

这是一个真实硬件驱动的学习与作品集项目，核心链路是 **STM32F103C8T6 + MPU9250 → I2C 采集 → 互补滤波 → UART/CH340 → PC 端处理与可视化**。

工程由两部分组成，可以各自独立使用，也共享同一份 CSV 数据格式：

| 部分 | 语言 | 职责 |
|---|---|---|
| `host/` | Python 3.10+ | 采集、记录、回放、校验、分析、出图、报告；一键流水线 |
| `cpp/` | C++17 + CMake | 协议解析、数据校验、跳变检测，以及多线程接收/处理/日志流水线 |

> 当前已完成范围：真实硬件连接、6 轴（加速度计 + 陀螺仪）数据读取、互补滤波 Pitch/Roll 输出和上位机三维可视化。当前固件没有实现磁力计读取 / Yaw 融合，也没有实现可靠的位移定位。文档会明确区分已完成和计划扩展内容。

## 一条命令跑完全流程

```bash
python -m pip install -r requirements.txt

# 全离线：生成数据 → 校验 → 归一化 → 分析 → 出图 → Markdown 报告
python -m host.pipeline --output-dir reports/demo

# 复用已有日志；或者只诊断一个可疑文件
python -m host.pipeline --source csv --input data/normalized.csv --output-dir reports/run2
python -m host.pipeline validate data/normalized.csv
```

产物：`normalized.csv`、`analysis.json`、`attitude_plot.png`、`report.md`、`pipeline.log`。
不需要硬件，不需要串口。详见 [`docs/PIPELINE.md`](docs/PIPELINE.md)。

## C++17 数据处理核心

```bash
cpp/scripts/build.sh Release
cpp/scripts/run_tests.sh                 # 128 个单测 + 稳定性扫描 + 队列基准

cpp/build/bin/imu_lab_cli stream --frames 20000 --workers 3 \
    --anomaly-rate 0.03 --step-deg 90 --csv out.csv --json metrics.json
```

接收线程 → 有界线程安全队列 → N 个解析线程 → 队列 → 日志线程；
统计处理耗时、队列积压、异常帧比例与内存占用。
设计见 [`docs/CPP_MODULE.md`](docs/CPP_MODULE.md)，
实测性能与扩展性分析见 [`docs/CPP_PERFORMANCE.md`](docs/CPP_PERFORMANCE.md)。

## 功能

- [x] STM32F103C8T6 Keil MDK 工程
- [x] I2C1 读取 MPU9250 加速度计、陀螺仪数据
- [x] 互补滤波输出 Pitch / Roll
- [x] USART1 文本数据输出（9600 baud）
- [x] Python 串口接收和 VPython 3D 模型显示
- [x] 真实硬件串口协议解析器及单元测试
- [x] 统一模拟数据、历史 CSV、真实串口三类数据输入
- [x] 将不同输入统一保存为标准 CSV，并统计无效/跳过记录
- [x] Git 忽略规则，避免提交虚拟环境和 Keil 编译产物
- [x] 实验数据 CSV 保存、离线统计与终端回放
- [x] 一键实验流水线：一条命令完成生成/导入 → 校验 → 分析 → 出图 → 报告
- [x] 输入数据校验：缺失字段、非法数字、空文件、异常文件，全部给出可定位的明细
- [x] 统一错误处理：串口不可用 / 文件不存在 / 格式错误都有稳定退出码与修复建议
- [x] GitHub Actions：Python 单测矩阵 + 流水线冒烟 + C++ 三平台构建 + sanitizer
- [x] C++17 数据处理核心（协议解析、校验、跳变检测），CMake 工程，128 个单测
- [x] C++ 多线程流水线：接收/处理/日志线程、有界线程安全队列、mutex + condition_variable
- [x] 性能与稳定性测试：处理耗时、队列积压、异常帧比例、内存占用、无丢帧不变量
- [x] Python 与 C++ 双向互操作（CI 中实测同一份 CSV 可互相读写）
- [x] 串口异常帧计数；[ ] 采样序号/心跳协议支持及精确丢帧统计
- [ ] I2C 故障恢复、采样时钟改进与算法误差评估
- [ ] 磁力计校准与 Yaw 解算

## 运行上位机

需要 Python 3.10+、串口转 USB 设备和已在工作的 STM32 固件。

```powershell
python -m pip install -r requirements.txt
python -m unittest discover -s tests -v
python -m host.visualize --port COM3 --baud 9600
```

## 记录、分析与回放实验

```powershell
# 记录 30 秒真实串口数据
python -m host.record --port COM3 --baud 9600 --duration 30 --output data/session.csv

# 查看姿态统计与大幅跳变提示
python -m host.analyze data/session.csv

# 按原始时间间隔回放，或 4 倍速回放
python -m host.replay data/session.csv
python -m host.replay data/session.csv --speed 4
```

详细说明见 [`docs/DATA_LOGGING_AND_REPLAY.md`](docs/DATA_LOGGING_AND_REPLAY.md)。

将 `COM3` 换成 Windows 设备管理器中 CH340 对应的串口号。波特率须与固件中的 `USART_BaudRate = 9600` 一致。启动前，关闭 XCOM 或其他已经打开同一个 COM 口的串口软件。

如果想保留你原来的 VPython 脚本，可对照 `docs/visualize_original.py.txt`；新的 `host/visualize.py` 做了协议解析拆分、命令行端口配置和异常行计数。

## 统一数据输入与采集

使用 `host.collect` 将不同来源的数据写入统一的 CSV 格式，之后可以共用分析和回放工具。

```powershell
# 不需要硬件：生成 5 秒模拟姿态数据
python -m host.collect --source simulated --duration 5 --hz 20 --output data/simulated.csv

# 读取已有 CSV 并规范化输出
python -m host.collect --source csv --input data/demo_attitude_offline.csv --output data/normalized.csv

# 有真实设备时：读取 STM32 串口
python -m host.collect --source serial --port COM3 --baud 9600 --duration 30 --output data/session.csv

# 对统一 CSV 继续分析或回放
python -m host.analyze data/simulated.csv
python -m host.replay data/simulated.csv --no-wait --limit 10
```

具体说明见 [`docs/UNIFIED_DATA_PIPELINE.md`](docs/UNIFIED_DATA_PIPELINE.md)。
模拟数据仅验证软件流程，不代表真实传感器性能；串口路径仍需硬件验证。

## 跑协议测试

```bash
# Python：96 个测试
python -m unittest discover -s tests -v

# C++：128 个测试 + 稳定性扫描 + 队列完整性
cpp/scripts/build.sh Release
cpp/scripts/run_tests.sh
```

这些自动化测试只测试协议解析、校验、统计与并发逻辑；它们不能代替硬件实测，也不能证明固件在所有传感器/接线条件下都稳定。
Python 与 C++ 两侧共用同一套判定规则，CI 里还有双向互操作任务实际跑一遍。

## 硬件与协议

- 接线：[`docs/HARDWARE_WIRING.md`](docs/HARDWARE_WIRING.md)
- 系统设计：[`docs/DESIGN.md`](docs/DESIGN.md)
- 完成情况及限制：[`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md)
- 迭代路线：[`docs/ROADMAP.md`](docs/ROADMAP.md)
- 固件说明：[`firmware/stm32f103/README.md`](firmware/stm32f103/README.md)
- 一键流水线：[`docs/PIPELINE.md`](docs/PIPELINE.md)
- C++ 模块设计：[`docs/CPP_MODULE.md`](docs/CPP_MODULE.md)
- C++ 性能实测：[`docs/CPP_PERFORMANCE.md`](docs/CPP_PERFORMANCE.md)

当前 MCU 输出格式是一行一帧：

```text
P:10.25,R:-5.50
```

其中 P 为俯仰角，R 为滚转角，单位为度。协议解析器也接受实验性 `X`、`Y` 扩展字段，但现有固件默认不输出位置数据；仅靠 MEMS 加速度双重积分得到的位置会快速漂移，不应作为可靠定位结果。

## 仓库目录

```text
imu-attitude-lab/
├── .github/workflows/         # CI：Python 单测/流水线、C++ 三平台、sanitizer、稳定性
├── firmware/stm32f103/        # 现有 Keil 工程与固件说明
├── host/imu_lab/              # 协议、校验、错误分类、数据源、统计与报告（可复用库）
│   ├── errors.py              #   统一异常层次 + 稳定退出码
│   ├── validation.py          #   行级数据校验
│   ├── reporting.py           #   统计与 Markdown 渲染
│   └── plotting.py            #   曲线图（matplotlib 可选依赖）
├── host/pipeline.py           # 一键实验流水线（本项目的主要入口）
├── host/visualize.py          # 对接真实 STM32 串口的 VPython 上位机
├── host/record.py             # 串口实验数据记录到 CSV
├── host/collect.py            # 统一模拟/CSV/串口三类输入到标准 CSV
├── host/analyze.py            # CSV 统计与角度跳变提示
├── host/replay.py             # CSV 实验时间回放
├── host/report.py             # 生成姿态曲线图与 Markdown 实验报告
├── cpp/                       # C++17 数据处理核心（独立 CMake 工程）
│   ├── include/imu_lab/       #   ← 公开头文件
│   ├── src/                   #   ← 实现
│   ├── tests/                 #   ← 128 个单元测试
│   ├── bench/                 #   ← 流水线与队列基准程序
│   └── scripts/               #   ← build.sh / run_tests.sh / debug.gdb
├── tests/                     # Python 单元测试
├── data/                      # 本地实验数据目录，不提交生成数据
├── docs/                      # 接线、设计、状态、迭代、流水线与 C++ 文档
└── requirements.txt
```

## 简历描述建议

可以根据实际复现情况写为：

> 基于 STM32F103C8T6 与 MPU9250 实现嵌入式姿态测量原型，完成 I2C 六轴传感器数据采集、加速度计/陀螺仪互补滤波及 Pitch/Roll 实时解算；通过 UART/CH340 建立上下位机通信，使用 Python、pyserial 和 VPython 开发实时三维姿态可视化工具，并补充串口协议解析测试与工程文档。

如果按"嵌入式 + Linux C++ + 机器人软件"方向投递，可以补上后半段（每一句本仓库都有对应代码与可复现的测试）：

> 使用 C++17 重写数据处理核心，采用 STL 与面向对象设计，通过 CMake 拆分为静态库 + CLI + 测试 + 基准四类目标，在 GCC/Clang/MSVC 三套工具链下开启 `-Werror` 构建；实现接收/处理/日志三线程流水线，基于 mutex 与 condition_variable 编写有界线程安全队列，并用序号重排保证"线程数只影响速度、不影响结果"；配套 128 个单元测试、ASan/UBSan/TSan 检测、以及统计处理耗时、队列积压、异常帧比例与内存占用的性能基准。

只能保留自己实际做过且能现场解释、复现的内容。不要将计划中的磁力计融合、Yaw、精度指标或可靠定位写成已经完成。

## 生成实验报告

一键流水线已经包含报告生成：

```bash
python -m host.pipeline --output-dir reports/demo
```

也可以只对已有 CSV 出报告：

```bash
python -m host.report data/normalized.csv --output-dir reports/run1 --data-kind simulated
```

输出包含 `attitude_plot.png` 与 `report.md`。完整说明见
[`docs/PIPELINE.md`](docs/PIPELINE.md) 与 [`docs/EXPERIMENT_REPORT.md`](docs/EXPERIMENT_REPORT.md)。
若数据是模拟的，请使用 `--data-kind simulated`；不要把模拟统计当作实机测量指标。
