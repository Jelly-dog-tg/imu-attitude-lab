# C++17 数据处理核心

`cpp/` 是独立的 CMake 工程，把上位机里"解析协议、校验数据、检测角度跳变"这几个
环节用 C++17 重做一遍，并加上真正并发化的处理流水线。
它不依赖 Python 部分，也不依赖任何第三方库——只需要标准库和 `Threads`。

## 为什么要有这一层

Python 版本适合做实验脚本，但求职方向是嵌入式 + Linux C++ + 机器人软件，
需要证明的是另一组能力：STL 与面向对象设计、CMake 工程组织、多线程同步、
内存与性能意识、以及能在 Linux 上编译和调试。所以这里不是"把 Python 翻译一遍"，
而是按 C++ 的约束重新设计。

## 目录结构

```text
cpp/
├── CMakeLists.txt              # 唯一构建入口，GCC/Clang/MSVC 通用
├── include/imu_lab/            # 公开头文件（对外只有这一层）
│   ├── error.hpp               # ErrorCode / Error / Result<T> / Status
│   ├── attitude_sample.hpp     # AttitudeSample / RawFrame / ParsedFrame
│   ├── number_parser.hpp       # locale 无关的十进制解析
│   ├── protocol_parser.hpp     # P:/R: 与 IMU,... 两种协议
│   ├── csv_reader.hpp          # CSV 读取 + 行级校验
│   ├── statistics.hpp          # Welford 在线统计
│   ├── jump_detector.hpp       # 角度跳变检测
│   ├── thread_safe_queue.hpp   # 有界 MPMC 队列（模板，头文件实现）
│   ├── frame_source.hpp        # 数据源抽象 + 模拟源 + 文件回放源
│   ├── metrics.hpp             # 耗时 / 积压 / 异常率 / 内存指标
│   ├── pipeline.hpp            # 多线程流水线
│   └── test_framework.hpp      # 零依赖测试框架（仅测试使用）
├── src/                        # 与头文件一一对应的实现
├── tests/                      # 9 个测试文件，128 个用例
├── bench/                      # 两个基准程序
└── scripts/                    # build.sh / run_tests.sh / debug.gdb
```

头文件与源文件严格分离：`include/` 里的内容是可以被外部消费的契约，
`src/` 里放实现。`(anonymous namespace)` 的内部类型不出现在头文件中。

## 关键设计决策

### 1. 错误用返回值，而不是异常

解析器在执行热路径上会被大量非法帧命中（串口噪声、截断帧、传感器给出的 NaN），
所以错误被建模成**值**：

```cpp
enum class ErrorCode { ok, empty_line, missing_separator, unknown_field, ... };

template <typename T> class Result {          // 有值时取值，失败时取 Error
    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] const Error& error() const&;
};
class Status {                                // 只表示成败
    [[nodiscard]] bool ok() const noexcept;
};
```

好处：不分配、不展开栈、失败点可枚举、测试能直接断言错误码。
`ErrorCode` 是有序枚举且有 `to_string()`，日志和报告里不会出现裸数字。

异常只在**违反前置条件**时使用（比如 `JumpDetector` 的阈值必须为正），
因为这属于调用方的编程错误，而不是数据问题。

### 2. 自己写十进制解析

`number_parser.hpp` 里的 `parse_double` 是手写的，原因有两条：

- `std::from_chars` 的**浮点**重载在若干工具链上仍然缺失
  （本机 MinGW-w64 GCC 11.2 的 libstdc++ 就没有），直接依赖它会导致
  "在别人的 Windows 上编译不过"。
- `strtod` / `std::stod` 受进程 locale 影响，在小数点用逗号的机器上
  会把 `"1.5"` 解析成 `1`。对采集数据来说这是静默的数据损坏。

手写版本用 `uint64_t` 累积有效数字（最多 19 位）、单独跟踪十进制指数、
再用精确的 10^n 查表缩放（超过 ±350 直接判为溢出/下溢）。
`nan` / `inf` / `infinity` 会被识别为"解析成功但非有限"，
这样上层能把它归类成 `not_finite` 而不是"乱码"。

### 3. 统计用 Welford 单遍算法

`RunningStats` 不保存任何样本，只维护 `count / mean / M2 / min / max`。
长时采集下"先缓存再求和平方"的写法会既吃内存又丢精度；
Welford 单遍更新在数据偏移量很大时依然稳定（测试里用 `1e9` 量级的
常量偏移专门验证过这一点）。

标准差用总体标准差，与 Python 的 `statistics.pstdev` 保持一致，
两个实现给出的数字才能直接对比。

### 4. 线程拓扑

```text
[receiver 线程]  ×1
    从 FrameSource 读帧、打上到达时间戳，push 进 raw_queue
                    │
                    ▼  ThreadSafeQueue<RawFrame>（有界）
[worker 线程]    ×N
    解析 + 校验，测量解析耗时，push 进 output_queue
                    │
                    ▼  ThreadSafeQueue<OutputItem>（有界）
[logger 线程]    ×1
    按序号重排 → 跳变检测 → 写 CSV → 累积统计 → 采样 RSS
```

三个设计要点：

1. **只有解析被并行**。解析是唯一吃 CPU 的步骤，其余都是 I/O 或顺序相关。
2. **跳变检测放在 logger 线程**。它比较相邻样本，是顺序相关的；
   如果放在 worker 上，结果就会依赖线程调度顺序——那是不可复现的 bug。
3. **关闭流程完全由队列关闭驱动**，不用 sleep、不用轮询：

   ```text
   receiver 读完 → raw_queue.close()
                → worker 排空并退出
                → 主线程 output_queue.close()
                → logger 排空并退出
   ```

   因此不会丢帧，也不会有线程在写文件时被强杀。

### 5. 顺序重排

多个 worker 完成的顺序不确定，但 CSV 的行序、跳变检测、统计累加都要求顺序一致。
logger 用一个以序号为键的待处理表，只在序号连续时才向下"放行"：

```cpp
auto drain_ready = [&] {
    for (auto it = pending.find(next_sequence); it != pending.end();
         it = pending.find(next_sequence)) {
        emit(std::move(it->second));
        pending.erase(it);
        ++next_sequence;
    }
};
```

线程数因此完全不影响结果——基准程序把"1/2/4 线程统计量逐位一致"做成了断言。

### 6. 有界队列与背压

队列容量默认为 4096，`push` 在满时阻塞。无界队列会把"消费者太慢"
变成一个内存耗尽崩溃；有界队列把它变成背压，这才是采集前端应有的行为。
水位峰值 `high_water_mark()` 作为"队列积压"指标直接输出。

## 构建与运行

### Linux / WSL2（主要目标）

```bash
sudo apt-get install -y build-essential cmake ninja-build gdb
cpp/scripts/build.sh Release            # 或 Debug（GDB 需要）
cpp/scripts/run_tests.sh
```

### Windows + MinGW-w64

```bash
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIMU_LAB_WERROR=ON
cmake --build cpp/build --parallel
cpp/build/bin/imu_lab_tests.exe
```

本仓库已在 MinGW-w64 GCC 11.2.0 + CMake 3.23 + Ninja 1.13 上完整验证。
二进制统一落在 `cpp/build/bin/`（多配置生成器也是），脚本和 CI 不需要猜路径。

### CMake 选项

| 选项 | 默认 | 作用 |
|---|---|---|
| `IMU_LAB_BUILD_TESTS` | `ON` | 构建单测与 CTest 用例 |
| `IMU_LAB_BUILD_BENCH` | `ON` | 构建 `imu_lab_bench` / `imu_lab_bench_queue` |
| `IMU_LAB_WERROR` | `OFF` | 警告即错误（CI 与 `scripts/build.sh` 打开） |
| `IMU_LAB_SANITIZER` | 空 | `address+undefined` 或 `thread` |

## CLI

```bash
# 生成并处理一个模拟流，写 CSV 与指标 JSON
imu_lab_cli stream --frames 20000 --hz 200 --workers 3 \
    --anomaly-rate 0.03 --step-deg 90 --csv out.csv --json metrics.json

# 回放一个抓包文件（每行一条 P:/R: 协议）
imu_lab_cli stream --input capture.txt --hz 100

# 统计已有日志
imu_lab_cli analyze out.csv --jump-threshold 30

# 行级校验报告
imu_lab_cli validate out.csv
```

退出码与 Python 侧完全一致，便于脚本和 CI 统一处理：
`0` 成功 / `1` 无数据 / `2` 参数错误 / `3` 数据非法 / `4` 输入缺失 /
`5` 数据源不可用 / `10` 内部错误。

## 测试

128 个用例，覆盖**合法输入、非法输入、边界值与错误处理**四类：

| 测试文件 | 关注点 |
|---|---|
| `test_number_parser.cpp` | 合法/非法字面量、溢出、下溢、非有限、locale 无关、超长数字不溢出累加器 |
| `test_protocol_parser.cpp` | 两种协议、每种拒绝路径的错误码、`\r\n`、字段顺序、往返格式化 |
| `test_csv_reader.cpp` | RFC4180 引号与转义、BOM、缺列、重复列、未知列、行列错位、issue 上限 |
| `test_statistics.cpp` | 与两遍参考实现对比、空/单样本、大偏移稳定性 |
| `test_jump_detector.cpp` | 阈值**严格大于**的边界、双通道、reset、非法阈值抛异常 |
| `test_thread_safe_queue.cpp` | FIFO、背压、`close()` 语义、高水位、4×3 并发无丢失无重复 |
| `test_metrics.cpp` | 延迟聚合、比值、除零保护、JSON 字段、RSS 可读 |
| `test_pipeline.cpp` | 端到端计数、确定性、线程数无关、小容量背压、故障注入、CSV 往返、错误码、提前停止 |

测试框架是自己写的（`test_framework.hpp`，约 200 行）：用例自注册、
断言失败给出文件行号与实际值、支持按子串过滤、失败时返回非零退出码。
这么做是为了**不依赖网络**——用 FetchContent 拉 GoogleTest 会让构建依赖外网，
而本机与部分 CI 环境的代理并不稳定。

## 用 GDB 调试

```bash
cpp/scripts/build.sh Debug

# Linux / WSL2
gdb -tui -x cpp/scripts/debug.gdb --args cpp/build/bin/imu_lab_cli stream --frames 500 --workers 3

# Windows / MinGW（Git Bash 需要关掉参数路径转换，并绕开 gdb 自带的 Python 助手）
MSYS2_ARG_CONV_EXCL='*' env -u PYTHONPATH -u PYTHONHOME \
  gdb -nx -x cpp/scripts/debug.gdb --args cpp/build/bin/imu_lab_cli.exe stream --frames 500 --workers 3
```

`debug.gdb` 预置了三个命令：

- `threadsummary`：列出所有线程并各打印两帧栈——最快看出某个线程卡在哪个阶段；
- `showstats`：打印 `run.stats` 与完整性计数；
- `trace-queue`：给队列两侧加自动继续的断点，用来确认生产者/消费者的交错。

默认只停一次（`AttitudePipeline::run` 入口），停下来后可以：

```gdb
print config_        # 线程数、队列容量、跳变阈值
info threads         # receiver / workers / logger 都在
continue
```

已验证的调试会话（本机 MinGW 实测）：断点命中在 `src/pipeline.cpp:115`，
`print config_.worker_threads` 返回 `3`，`info threads` 能看到主线程与后台线程。

## 与 Python 侧的互操作

两侧共用同一张 CSV schema：

```text
timestamp_utc,elapsed_s,pitch_deg,roll_deg,x,y,raw_line
```

`raw_line` 含逗号，因此写出时必须加引号；C++ 侧实现了与 Python `csv` 模块
一致的引号规则（仅在需要时加引号、内部引号翻倍）。CI 里有双向验证：

- C++ `stream --csv` 写出 → `python -m host.pipeline validate` / `host.analyze` 读取；
- `python -m host.pipeline` 写出 → `imu_lab_cli validate` / `analyze` 读取。

实测两侧对同一份 3000 行日志都报告 3000 valid / 0 invalid，
均值与标准差一致（见 `docs/CPP_PERFORMANCE.md`）。
