# 项目状态与可复现范围

## 已完成并有用户实际硬件/上位机工作记录支持

- 使用 STM32F103C8T6、MPU9250 搭建硬件原型。
- Keil MDK 工程配置、固件下载与调试链路。
- I2C 读取加速度计、陀螺仪原始数据。
- 互补滤波计算 Pitch / Roll。
- 通过 USART1 + CH340 向 PC 发送角度文本。
- Python `pyserial` 读取串口，并使用 VPython 进行三维姿态可视化。

## 已完成且可在无硬件环境下复现（P0 工程化）

- **一键实验流水线** `python -m host.pipeline`：数据生成/导入 → 校验 → 归一化 →
  分析 → 出图 → Markdown 报告，产物包括 `normalized.csv` / `analysis.json` /
  `attitude_plot.png` / `report.md` / `pipeline.log`。见 `docs/PIPELINE.md`。
- **输入数据校验** `host/imu_lab/validation.py`：文件级（不存在/目录/0 字节/只有表头/
  缺列/重复列/未知列）与行级（字段缺失/为空/非数字/NaN/负 elapsed/字段数超表头/
  x-y 不成对/时间倒流/时间戳重复）两类检查，逐条给出错误码、行号与列名，
  并有 `--max-issues` 上限保护。
- **统一错误处理** `host/imu_lab/errors.py`：所有面向用户的失败都是 `ImuLabError`
  子类，携带稳定退出码（0/1/2/3/4/5/10）与可执行的修复建议；
  串口不可用、文件不存在、格式错误都有专门分类。
- **自动化测试与 CI**：Python 96 个测试（原 28 个 + 新增 68 个）；
  `.github/workflows/ci.yml` 覆盖 Python 单测矩阵、离线流水线冒烟与产物断言、
  C++ 三平台构建与单测、ASan/UBSan/TSan、稳定性扫描与双向互操作。

## 已完成且可在无硬件环境下复现（P1 C++ / Linux）

- **C++17 数据处理核心**（`cpp/`）：独立的 CMake 工程，头文件与源文件分离，
  只用标准库 + `Threads`，不下载任何第三方依赖。
  模块包括协议解析（`P:/R:` 与 `IMU,...` 两种格式）、CSV 读取与行级校验、
  Welford 在线统计、角度跳变检测、locale 无关的十进制解析。
- **多线程流水线**：接收线程 → 有界 `ThreadSafeQueue<RawFrame>` → N 个解析线程 →
  有界 `ThreadSafeQueue<OutputItem>` → 日志线程。跳变检测因为顺序相关放在日志线程，
  配合序号重排，保证**线程数只影响速度、不影响结果**。
- **128 个 C++ 单元测试**，覆盖合法输入、非法输入、边界值与错误处理；
  含 4 生产者 × 3 消费者并发压测（校验和精确匹配，无丢失无重复）。
- **性能与稳定性测试** `imu_lab_bench` / `imu_lab_bench_queue`：统计处理耗时、
  队列积压水位、异常帧比例、吞吐、内存占用（Windows `GetProcessMemoryInfo` /
  Linux `/proc/self/statm`），并把"无丢帧、统计量线程无关"做成断言。
  实测数据与瓶颈分析见 `docs/CPP_PERFORMANCE.md`。
- **GCC / GDB**：本机用 MinGW-w64 GCC 11.2 完成构建、单测与 GDB 断点调试验证；
  `cpp/scripts/debug.gdb` 提供 `threadsummary` / `showstats` / `trace-queue` 三个命令。
  Ubuntu/WSL2 的构建与调试步骤见 `docs/CPP_MODULE.md`。
- **Python ↔ C++ 互操作**：两侧共用同一张 CSV schema（含 `raw_line` 引号规则），
  CI 中双向实测（C++ 写 → Python 读；Python 写 → C++ 读）。

## 仍需后续补齐的工程质量证据

- 加速度计/陀螺仪标定、滤波参数对比和标准参考下的误差评估。
- I2C 超时、传感器掉线、非法数据的故障处理（固件侧）。
- 磁力计校准、倾斜补偿、Yaw 解算（尚未在当前固件中实现）。
- 位置估计若依赖加速度双重积分，只能作为漂移实验；不能描述为可靠定位能力。
- 当前固件的 P/R 行协议没有帧序号，因此不能准确统计固件侧丢帧；
  平均采样率可从录制时间戳估算。串口路径的端到端验证仍需硬件。
- C++ 流水线在 Windows/MinGW 上吞吐随线程数下降（同步开销主导，
  解析单帧仅约 300 ns）。Linux 的对照数据由 CI 产出，
  优化方向（批量交接、无锁环形缓冲、自适应等待）记录在 `docs/CPP_PERFORMANCE.md`。

## 未开始（P2 机器人与边缘 AI 扩展）

按优先级排在这些之后，均未实现：

- ROS2 节点：发布与订阅姿态数据。
- OpenCV 数据可视化或传感器数据接入。
- ONNX Runtime 模型推理接口。
- Docker 环境封装。
- 磁力计校准与 Yaw 解算（需真实硬件才能验证）。

## 与其他代码的边界

原压缩包中的 `python_work/project_Gemini/` 包含阈值法、峰值法、恒比定时、小波降噪和误差分析等信号处理脚本。它看起来是另一组信号处理工作，不是当前 IMU Pitch/Roll 程序的直接依赖。建议将其作为独立项目或独立仓库维护，不把整个旧工作目录混入这个仓库。
