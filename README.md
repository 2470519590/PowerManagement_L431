# L431 电源管理

## 供电通道

PCB 丝印中的 `AMMO` 通道在本机上实际接到底盘，以下将其称为“底盘通道”。系统上电后打开底盘通道，持续采集该路电流并计算功率。

`GIMBAL`、`CHASSIS`、`MINI-PC` 三路目前未焊接使用，并保持关闭。后续计划从这三路中焊接一路，为云台发射机构供电；该通道只需要开关控制，不纳入电流采样和功率限制。

## 功率缓冲与断电保护

功率限制仅作用于底盘通道，采用 RoboMaster 地面机器人同类的缓冲能量规则：

- 缓冲能量初始值和上限均为 `20 J`。
- 电流和功率以 `1 kHz` 更新；每 `100 ms` 结算一次。
- 功率以 `1 kHz` 计算，功率上限与缓冲能量每 `100 ms` 结算一次。结算使用该时刻的功率值与功率上限比较。超过 `28 W` 时，缓冲减少 `(功率 - 28 W) × 0.1 s`；未超限时按相同方式回充，最高回充至 `20 J`。
- 缓冲已耗尽且该次结算仍超限时，底盘通道断电 `5 s`；到时后自动恢复供电。

当前暂时关闭功率限制和缓冲能量限制，相关计算代码保留；电流保护仍然启用。默认过流保护为 `4 A`：连续 `200 ms` 的 1 kHz 电流样本达到或超过阈值时，底盘立即断电并锁存过流状态。功率、过流、缓冲上限和断电时长均在 `Application/Power/app_power_config.h` 配置。

LPUART1 使用 `115200, 8N1`。状态帧默认关闭，可按需开启 10 Hz 上报或读取最近一次结算结果；功率上限、缓冲能量、过流阈值和调试开关也通过该串口完成。具体格式见 [电管串口通信命令](docs/电管串口通信命令.md)。

保护状态变化时，电管会通过同一串口主动通知小车主控：发送 `FIRE=OFF` 时主控立即禁止发射；保护解除且底盘恢复供电时发送 `FIRE=ON`，主控才恢复发射。状态帧中的 `FIRE` 字段用于同步当前发射许可状态。

CAN 外设驱动已由 CubeMX 生成，但接口尚未焊接，当前没有 CAN 通信业务或 CAN 协议。底盘主控与电管的联调使用 LPUART1。

## 硬件与换算

- MCU：`STM32L431RC`
- 电流检测：`INA180A2IDBVR`，增益 `50 V/V`
- 采样电阻：`4 mΩ`
- 底盘电流 ADC：`PA5 / AMMO-U / ADC1_IN10`
- 功率计算使用电压 `11.98 V`

系统按 12 V 电池使用场景计算功率，当前硬件未预留母线电压采样。`INA180A2IDBVR` 的输出在高电流时受 3.3 V 供电影响：功率超过 `160 W` 时，串口 `STA` 的 `SAT=1`，表示后续读数可能接近饱和；按器件输出摆幅估算，约 `168 W` 以内可保证测量精度，该上限尚未实测。

原理图中 `INT-AMMO` 经 `R44` 驱动控制 MOS，控制节点由 `R43 (72 kΩ)` 上拉。因此 MCU 复位或失电、引脚高阻时，底盘通道默认关闭。

固件启用 STM32 独立看门狗，标称超时约 `1 s`；仅当主循环完成一轮通信任务后才喂狗。程序跑飞时 MCU 复位，底盘保持关闭，由主控发送 `REQ,CHS,ON` 完成自检后恢复。J-Link 暂停调试时看门狗冻结。更高可靠性仍需外部看门狗或独立硬件失效关断。

```text
VOUT = I × 0.004 Ω × 50 = I × 0.2 V
I(mA) = VOUT(mV) × 5

VOUT(mV) = adc_raw × 3300 / 4095
```

## Python 工具与遥测

工具位于 `tool/`，使用 Conda 环境 `py310`。

安装依赖：

```powershell
conda run -n py310 python -m pip install -r tool\requirements.txt
```

### 蓝牙遥测程序

`tool/bluetooth_power_monitor.py` 用于电脑端实时查看电管状态，显示 100 ms 平均电流、平均功率、缓冲能量、功率限制和保护状态；功率超限 Cut、过流/堵转保护触发时弹窗提示。

电脑端使用步骤：

1. 电脑先在系统蓝牙设置中与 `JDY-31-SPP` 配对。
2. 确认蓝牙串口已经出现在 Windows 的 COM 端口中。
3. 启动程序，在下拉框选择该蓝牙串口。
4. 点击“开始”，程序自动发送 `STA=ON` 并开始记录。
5. 点击“停止”，程序发送 `STA=OFF`、关闭串口并结束记录。

```powershell
conda run -n py310 python tool\bluetooth_power_monitor.py
```

每次开始记录会在 `tool/log/` 自动生成以电脑开始时间命名的日志文件：

```text
realtime_log_YYYYMMDDHHMMSS.txt
```

超过 3 秒没有收到有效 `STA` 状态帧时，程序会自动停止记录并弹窗提示。

手机端调试可使用 `SPP蓝牙串口` APP：先与 `JDY-31-SPP` 配对，打开蓝牙串口后设置波特率 `115200`，发送：

```text
STA=ON
```

即可实时接收 10 Hz 状态帧。命令、状态字段和数据含义见 [电管串口通信命令](docs/电管串口通信命令.md)。

### 日志曲线分析程序

`tool/plot_power_curve.py` 读取遥测日志中的 `STA` 状态帧，绘制功率、电流和缓冲能量曲线，并标出当前 `28 W` 功率限制线和 `4 A` 电流保护参考线；日志很长时会自动进行分桶压缩。

```powershell
conda run -n py310 python tool\plot_power_curve.py tool\log\realtime_log_YYYYMMDDHHMMSS.txt
```

日志也可以导出 CSV，或基于原始 `PAVG` 数据自行编写 Python 脚本进行进一步分析：

```powershell
conda run -n py310 python tool\plot_power_curve.py tool\log\realtime_log_YYYYMMDDHHMMSS.txt --csv tool\log\power_curve.csv
```
