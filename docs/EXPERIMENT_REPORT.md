# 自动生成实验报告

当 CSV 已经由 `host.collect` 或 `host.record` 生成后，可以把角度曲线和统计信息输出到文件。

```powershell
# 对模拟数据生成报告，标明数据来源为 simulated
python -m host.report data/simulated.csv --output-dir reports/simulated --data-kind simulated

# 对真实硬件记录生成报告（前提是这份 CSV 确实来自实机）
python -m host.report data/session.csv --output-dir reports/session --data-kind real

# 调整相邻角度变化的提醒阈值
python -m host.report data/simulated.csv --output-dir reports/custom --jump-threshold-deg 20 --data-kind simulated
```

每次生成会输出两个文件：

- `attitude_plot.png`：Pitch 和 Roll 随时间变化的曲线；超过阈值的相邻样本用叉号标记。
- `report.md`：样本数、坏行数、持续时间、估算采样率、最小/最大/均值/标准差和跳变明细。

## 如何解释结果

- 平均采样率由 CSV 中的 elapsed 时间戳估算，不等于经过硬件工具测量的 MCU 定时器精度。
- 标准差反映样本分布，不等于相对于真实姿态基准的测量误差。
- 超阈值跳变只是排查提示，真实运动也可能造成大变化。
- 模拟数据报告中的任何数值都不能作为真实传感器性能或精度证明。

## 依赖与测试

将 `matplotlib>=3.8` 添加到项目 `requirements.txt`，然后执行：

```powershell
python -m pip install -r requirements.txt
python -m unittest discover -s tests -v
```
