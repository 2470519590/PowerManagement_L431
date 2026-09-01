# L431 电源管理

## 当前业务

PCB 的 `AMMO` 通道实际供底盘使用，当前所有电源管理业务均针对底盘。上电后底盘默认打开，持续采集电流并计算功率。

其余三路未使用，保持关闭。后续预留通道只控制开关，不参与采样和功率限制。

当前默认关闭功率限制和缓冲能量限制，代码保留；电流保护保持启用：

- 电流保护：`4 A`，持续 `200 ms` 触发断电
- 功率限制：`28 W`，当前关闭
- 缓冲能量：`20 J`，当前关闭
- 保护断电：`5 s` 后恢复

## 硬件

- MCU：`STM32L431RC`
- 电流检测：`INA180A2IDBVR`，增益 `50 V/V`
- 采样电阻：`4 mΩ`
- 底盘采样：`PA5 / ADC1_IN10`
- 电池电压按 `11.98 V` 计算
- 串口：`LPUART1`，`115200 8N1`
- CAN 接口未焊接，保留 CAN 驱动

INA180 输出接近 3.3 V 时会饱和。功率超过约 `160 W` 后读数可能失真，理论约 `168 W` 以内可保证精度，尚未实测。

## 计算说明

ADC 使用 12 位结果：

```text
VOUT(mV) = ADC原始值 × 3300 / 4095
I(mA) = VOUT(mV) / (50 × 0.004 Ω) = VOUT(mV) × 5
P(W) = I(mA) × 11980 / 1000000
```

电流和功率每 `1 ms` 计算一次，即 `1 kHz`。每 `100` 个有效采样计算一次平均电流和平均功率；功率峰值取这 `100 ms` 内的最大瞬时功率。当前功率限制和缓冲限制关闭，因此仍会计算并上报功率，但不会因功率超限断电。

电流保护按每个有效采样判断，达到 `4 A` 后累计 `200` 个连续采样触发。ADC 启动失败不会计入有效采样，也不会参与平均值；连续失败达到配置阈值时关闭底盘电源。

功率、电流均使用整数运算，显示值会向下取整。功率计算使用固定 `11.98 V`，硬件没有母线电压采样，因此不会反映电池实时电压变化。

MCU 复位或失电时，硬件上拉使底盘电源关闭。固件启用独立 IWDG，主循环异常时复位。

## 串口和工具

串口命令见 [电管串口通信命令](docs/电管串口通信命令.md)。10 Hz 状态帧默认关闭，发送 `STA=ON` 开启；发送 `PING` 返回 `PONG`。

Python 工具使用 Conda `py310`：

```powershell
conda run -n py310 python -m pip install -r tool\requirements.txt
conda run -n py310 python tool\bluetooth_power_monitor.py
```

`bluetooth_power_monitor.py` 用于蓝牙实时查看和记录，先配对 `JDY-31-SPP`，再选择蓝牙 COM 口并点击开始。

`plot_power_curve.py` 用于根据日志绘制功率、电流和缓冲曲线：

```powershell
conda run -n py310 python tool\plot_power_curve.py tool\log\realtime_log_YYYYMMDDHHMMSS.txt
```
