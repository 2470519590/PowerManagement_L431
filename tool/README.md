# 蓝牙串口电管监视器

蓝牙模块配对后会分配一个 Windows 串口。工具启动后先在下拉框选择串口，点击“开始”后打开串口并自动订阅 `STA=ON`；点击“停止”结束记录。为兼容打开串口时复位 MCU 的 USB 串口桥，首次订阅延迟 500 ms；未收到状态帧前每秒重试一次。

功率缓冲耗尽触发 5 秒断电，或过流/堵转保护触发时，窗口会响铃并弹出提示；同一事件持续期间只提示一次。

```powershell
conda run -n py310 python -m pip install -r tool\requirements.txt
conda run -n py310 python tool\bluetooth_power_monitor.py --list
conda run -n py310 python tool\bluetooth_power_monitor.py --port COM7
```

每次点击“开始”都会在 `tool/log` 创建 `realtime_log_YYYYMMDDHHMMSS.txt`，文件名使用开始记录时的电脑本地时间；串口收到的文本、发送的命令和连接状态都会写入日志。点击“停止”后关闭串口并结束文件写入。

超过 3 s 未收到有效 `STA,...` 状态帧时，工具会自动停止记录、关闭串口并弹窗提示。电管串口必须为 `115200, 8N1`，且使用 `STA=...` 状态帧协议。

## 绘制历史功率曲线

日志可以包含命令、ACK、连接提示和大量状态帧。绘图程序只解析 `STA,...` 行，按 10 Hz 状态帧计算时间，并对超长日志进行分桶极值压缩。

```powershell
conda run -n py310 python tool\plot_power_curve.py tool\log\realtime_log_20260826003108_1787675468..txt
conda run -n py310 python tool\plot_power_curve.py tool\log\realtime_log_20260826003108_1787675468..txt --csv tool\log\power_curve.csv
```

输出 PNG 默认与输入日志同名；图中包含功率、35 W 历史参考线、电流、8 A 历史参考线和剩余缓冲能量。`--max-points` 可限制绘图点数，默认 20000。

从指定时间开始按固定时长分图，例如从 100 s 开始，每 50 s 输出 5 张图：

```powershell
conda run -n py310 python tool\plot_power_curve.py tool\log\realtime_log_20260826003108_1787675468..txt --start-time 100 --window-seconds 50 --count 5
```

输出范围为 `100–150 s`、`150–200 s`、`200–250 s`、`250–300 s`、`300–350 s`。

## 重算不同功率限制下的缓冲曲线

使用原始日志中的 `PAVG`，按每 `100 ms` 结算、初始缓冲 `60 J` 重算历史数据。示例默认计算 `150–200 s`，功率限制为 `30/25/20/15 W`；这是历史分析脚本参数，不代表当前固件默认值：

```powershell
conda run -n py310 python tool\recalculate_buffer_curve.py tool\log\realtime_log_20260826003108_1787675468..txt
```

例如以 `20 J` 作为初值和全局上限，分别绘制 `25 W`、`30 W` 两张图：

```powershell
conda run -n py310 python tool\recalculate_buffer_curve.py tool\log\realtime_log_20260826003108_1787675468..txt --limits 25 30 --initial-buffer 20 --buffer-max 20 --separate
```
