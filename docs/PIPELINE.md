# 一键实验流水线

`python -m host.pipeline` 把"数据生成/导入 → 校验 → 归一化 → 分析 → 出图 →
Markdown 报告"合并成一条命令。整个流程可以完全离线运行，不需要任何硬件。

## 快速开始

```bash
python -m pip install -r requirements.txt

# 全离线演示：生成模拟数据并产出完整报告
python -m host.pipeline --output-dir reports/demo

# 复用已有日志
python -m host.pipeline --source csv --input data/normalized.csv --output-dir reports/run2

# 只诊断一个可疑文件（不跑流水线）
python -m host.pipeline validate data/normalized.csv
```

## 阶段与产物

```text
input-check → acquire → validate → analyze → plot → report
```

| 阶段 | 做什么 | 何时被跳过 |
|---|---|---|
| `input-check` | 对用户传入的 CSV 做行级校验 | `--source simulated/serial` 时跳过 |
| `acquire` | 从数据源取样本，写成标准 CSV | 永不需要 |
| `validate` | 再校验一遍产出物本身 | 永不需要 |
| `analyze` | 统计量 + 跳变检测，写 `analysis.json` | 永不需要 |
| `plot` | 生成 Pitch/Roll 曲线图 | matplotlib 未安装时跳过（可 `--require-plot` 改为失败） |
| `report` | 渲染 `report.md` | 永不需要 |

输出目录内容：

| 文件 | 说明 |
|---|---|
| `normalized.csv` | 标准 schema 的清洗后日志 |
| `analysis.json` | 机器可读的统计量与跳变事件 |
| `attitude_plot.png` | 双通道曲线图（大跳变用 × 标出） |
| `report.md` | 实验报告：统计表、跳变表、流水线汇总、校验汇总 |
| `pipeline.log` | 每个阶段的 JSON Lines 事件流（含耗时） |

`plot` 阶段失败不会让整条流水线失败——报告会照常产出，只是不嵌入图片。
这个选择是有意的：一份没有图的报告仍然有分析价值，而让整个流程因为
一个可选依赖缺失而失败，会把"装了 matplotlib 吗"变成能不能出结果的前提。

## 数据校验

`host/imu_lab/validation.py` 独立于加载器，专门负责"说清楚哪里坏了"。
加载器（`csvlog.load_csv`）是宽容的——它静默跳过坏行以便分析能继续；
校验器是**严格且啰嗦的**，这才是排障时需要的东西。

文件级检查：

- 文件不存在 / 是目录 / 0 字节；
- 只有表头没有数据行；
- 必需列缺失（`timestamp_utc` / `elapsed_s` / `pitch_deg` / `roll_deg`）；
- 表头重复列名；
- 未知列（仅告警）。

行级检查：

| 场景 | 错误码 |
|---|---|
| 字段缺失（该行比表头短） | `missing_column_in_row` |
| 必需字段为空 | `empty_field` |
| 不是数字 | `not_numeric` |
| NaN / inf | `not_finite` |
| `elapsed_s` 为负 | `negative_elapsed` |
| 比表头多出字段（通常是 `raw_line` 里的逗号没加引号） | `too_many_fields` |
| `x` / `y` 只出现一个 | `incomplete_position_pair` |
| 时间倒流 | `time_not_monotonic`（告警） |
| 时间戳重复 | `duplicate_timestamp`（告警） |

告警不阻断流程：部分损坏的日志仍然可用，这符合实验数据的真实情况。
严重等级分三档（`fatal` / `error` / `warning`），`ValidationReport.ok`
表示"能用"。

`--max-issues` 限制保留的明细条数（默认 200），避免一个 10M 行的坏文件
在校验阶段把内存吃光；被丢弃的条数记在 `truncated_issues` 里，
**计数永远精确**，只有明细会被截断。

## 统一错误处理

所有面向用户的失败都是 `host/imu_lab/errors.py` 里的 `ImuLabError` 子类，
每个都带**稳定的退出码**和可执行的修复建议：

| 退出码 | 含义 | 典型场景 |
|---:|---|---|
| 0 | 成功 | — |
| 1 | 没有可用数据 | 空文件、全部行被拒、串口一条有效帧都没有 |
| 2 | 参数错误 | `--source csv` 没给 `--input`、阈值为 0 |
| 3 | 数据非法 | 缺列、非法数字 |
| 4 | 输入缺失 | 文件不存在 |
| 5 | 数据源不可用 | 串口打不开、pyserial 未安装 |
| 10 | 内部错误 | 预期外的异常 |

两种情况都会给出"怎么办"，而不是把 traceback 甩给用户：

```text
$ python -m host.analyze data/missing.csv
host.analyze: input CSV not found: data/missing.csv
How to fix:
  - check the path for typos and use forward slashes or quotes on Windows
  - generate a sample dataset first: python -m host.pipeline --source simulated --output-dir reports/demo
```

```text
$ python -m host.collect --source serial --port COM9
host.collect: cannot open or read serial port 'COM9': could not open port 'COM9': ...
How to fix:
  - close other programs that may hold the port (Arduino IDE, PuTTY, another recorder)
  - confirm the USB-serial driver is installed and the port name matches (COM3, /dev/ttyUSB0)
  - list available ports: python -c "import serial.tools.list_ports as p; print([x.device for x in p.comports()])"
  - run with --source simulated to continue without hardware
```

实现上分两层：库代码（`protocol.py` / `csvlog.py` / `reporting.py`）继续抛
`ValueError`，保持易测；边界层（各 CLI 的 `main`、流水线）负责翻译。
`UsageError` 和 `DataFormatError` 同时继承 `ValueError`，
所以既有调用方与既有测试不需要改动。

所有 CLI 入口都经过同一个边界函数：

```python
def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return guarded("host.analyze", lambda: _analyze(args))
```

`guarded()` 负责分类异常、打印提示、返回退出码；
`KeyboardInterrupt` 依然向上传播，Ctrl+C 保持即时响应。

## 常用参数

| 参数 | 作用 |
|---|---|
| `--source simulated\|csv\|serial` | 数据来源 |
| `--input PATH` | `--source csv` 时的输入文件 |
| `--output-dir DIR` | 产物目录 |
| `--duration` / `--hz` / `--seed` | 模拟数据的时长、频率、随机种子（固定种子 → 可复现） |
| `--jump-threshold-deg` | 跳变判定阈值，默认 30° |
| `--data-kind auto\|real\|simulated\|historical\|unknown` | 写进报告的数据来源标注 |
| `--strict` | 有任何坏行就让 `input-check` 失败（默认保留有效行） |
| `--require-plot` | matplotlib 缺失时视为失败 |
| `--quiet` | 只打印最终汇总 |

`--data-kind` 的 `auto` 会先看数据源：`simulated` → 标注为模拟，
`serial` → 标注为真实，`csv` → 再看文件名里是否含 `simulat`/`demo`/`mock`。
这一步很关键：**模拟数据的统计绝不能被当成实机测量指标**，
报告里会显式给出 `synthetic` 警告。

## 测试

`tests/test_pipeline.py` 是端到端测试，全部离线运行，覆盖：

- 阶段序列与状态（含 `plot` 的"成功或优雅跳过"二选一）；
- 每个产物都产出且可解析，`analysis.json` 能被 `json.loads`；
- 归一化 CSV 的 schema 正确；
- `--source csv` 的输入校验、部分保留、`--strict` 快速失败；
- 错误分类：缺文件 → 4、空文件 → 1、列名错 → 3、串口不可用 → 5；
- 输入与输出同路径时拒绝执行（避免覆盖源文件）。

## GitHub Actions

`.github/workflows/ci.yml` 的 `python` 任务在 3.10 与 3.12 上：

1. 跑全部单测；
2. 跑一次完整的离线流水线；
3. 断言 5 个产物都存在且非空（不是"命令没报错"就算过）；
4. 断言校验坏文件时退出码确实是 3；
5. 上传 `reports/ci/` 作为 artifact。

`cpp-stability` 任务另外跑 Python ↔ C++ 的双向互操作，
确保两个实现共用同一份 schema（详见 `docs/CPP_MODULE.md`）。
