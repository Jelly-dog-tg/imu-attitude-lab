# imu_lab C++17 core

独立于 Python 上位机的 C++17 数据处理核心：串口协议解析、数据校验、
角度跳变检测，以及一条真正并发化的接收/处理/日志流水线。

只用标准库 + `Threads`，**不下载任何第三方依赖**（测试框架也是自带的），
因此在代理受限的环境里也能直接构建。

## 快速开始

```bash
# Linux / WSL2（主要目标）
sudo apt-get install -y build-essential cmake ninja-build gdb
scripts/build.sh Release
scripts/run_tests.sh

# Windows + MinGW-w64 / Git Bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIMU_LAB_WERROR=ON
cmake --build build --parallel
build/bin/imu_lab_tests.exe
```

二进制统一输出到 `build/bin/`：

| 程序 | 作用 |
|---|---|
| `imu_lab_cli` | 命令行入口：`stream` / `analyze` / `validate` |
| `imu_lab_tests` | 128 个单元测试（`--list` 列出，加子串可过滤） |
| `imu_lab_bench` | 流水线稳定性/性能基准，`--sweep` 对比 1/2/4 线程 |
| `imu_lab_bench_queue` | 队列微观基准，用来定位性能瓶颈 |

```bash
build/bin/imu_lab_cli stream --frames 20000 --workers 3 --anomaly-rate 0.03 \
    --step-deg 90 --csv out.csv --json metrics.json
build/bin/imu_lab_cli analyze out.csv
build/bin/imu_lab_cli validate out.csv

build/bin/imu_lab_bench       --frames 500000 --sweep --anomaly-rate 0.02
build/bin/imu_lab_bench_queue --items 1000000
```

## 模块一览

```text
include/imu_lab/
├── error.hpp              错误码 / Error / Result<T> / Status
├── attitude_sample.hpp    样本与帧类型
├── number_parser.hpp      locale 无关的十进制解析
├── protocol_parser.hpp    P:/R: 与 IMU,... 两种协议
├── csv_reader.hpp         CSV 读取 + 行级校验
├── statistics.hpp         Welford 在线统计
├── jump_detector.hpp      角度跳变检测
├── thread_safe_queue.hpp  有界 MPMC 队列
├── frame_source.hpp       数据源抽象（模拟 / 文件回放）
├── metrics.hpp            耗时 / 积压 / 异常率 / 内存
└── pipeline.hpp           多线程流水线
```

线程拓扑与设计取舍见 [`../docs/CPP_MODULE.md`](../docs/CPP_MODULE.md)，
实测性能与扩展性分析见 [`../docs/CPP_PERFORMANCE.md`](../docs/CPP_PERFORMANCE.md)。

## 一句话说明并发设计

接收线程 → `ThreadSafeQueue<RawFrame>` → N 个解析线程 → `ThreadSafeQueue<OutputItem>`
→ 日志线程。只有解析被并行；跳变检测因为比较相邻样本、顺序相关，
放在日志线程上执行，这样**线程数只影响速度，不影响结果**——
基准程序把这条不变量做成了断言。
