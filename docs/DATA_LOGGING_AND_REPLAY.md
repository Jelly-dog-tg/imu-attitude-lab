# 实验数据记录、分析与回放

## 1. 记录真实硬件数据

先确认 STM32 正在输出 `P:<pitch>,R:<roll>`，并关闭其他占用串口的程序：

```powershell
python -m host.record --port COM3 --baud 9600 --duration 30 --output data/session.csv
```

参数说明：`--duration` 为最多记录秒数；也可以用 `--max-samples 500` 在记录到指定有效样本数后停止。`data/*.csv` 默认在 `.gitignore` 中忽略，实验数据留在本机，不会意外提交到仓库。

CSV 每行包含 UTC 时间、相对起始时间、Pitch、Roll、可选 X/Y 和原始串口行。异常串口行会被跳过并计数，不会作为有效测量写入 CSV。

## 2. 分析记录

```powershell
python -m host.analyze data/session.csv
python -m host.analyze data/session.csv --jump-threshold-deg 20
```

工具输出样本数、无效 CSV 行数、持续时间、平均采样率，以及 Pitch/Roll 的最小值、最大值、均值、标准差。相邻角度跳变量超过阈值时会标记为“大幅跳变”。这只是辅助排查线索，不代表一定发生了传感器故障；真实运动也可能产生较大变化。

## 3. 回放实验

```powershell
python -m host.replay data/session.csv
python -m host.replay data/session.csv --speed 4
python -m host.replay data/session.csv --no-wait --limit 100
```

回放按 CSV 的相对时间间隔输出姿态角，支持加速播放、立即输出和限制行数。当前版本是终端回放，不会自动控制 VPython 模型。

## 4. 测试

```powershell
python -m unittest discover -s tests -v
```

CSV 单元测试不需要连接 STM32。测试通过只代表数据记录/加载逻辑满足测试用例，不替代真实串口长时间稳定性测试。
