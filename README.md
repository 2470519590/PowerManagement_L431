# L431 电源管理

## 三板联调位置与当前状态

本工程是比赛状态的权威端：维护 HP、存活状态、枪管热量、底盘平均功率和射击许可；它不直接联网，也不连接 Xbox 手柄。

```text
装甲板 ×4 ── CAN1 500 kbps ──> L431PM ── USART2 115200 ──> ESP32-S3 ── Wi-Fi ──> 服务器
枪管 ───────── CAN1 500 kbps ──>   │
                                      └── LPUART1 115200：主控命令与电源调试 ASCII 协议
```

当前代码已接入并在主循环调用：

- `Application/Referee/app_armor_enum.c`：四块装甲板 NodeID 枚举、分配和 ACK。
- `Application/Referee/app_referee_can.c`：枪管 `0x230/0x232`、装甲板动态业务 ID 的接收与 10 Hz 查询；枪管查询与装甲轮询错开；任一已分配装甲板持续离线会自动发起新一轮枚举。
- `Application/Referee/app_match.c`：每 100 ms 向 USART2 发比赛状态（含底盘供电状态）、转发攻击/脱战/受击等事件；每次有效 HIT 扣 20 HP；比赛开始后 HP 初始化为 300、请求枪管清热量，HP 为 0 后 5 秒复活至 300；接受 ESP32 的开始、结束、设置 HP、黄牌处罚和通电/断电命令并回执。死亡/复活在 USART2 上等待 ESP32 ACK 后重传。黄牌每次扣 50 HP，第 3 次扣血后立即判负、断电且不再复活。
- `Bsp/bsp_uart2.c`：USART2 的中断收发队列，PA2=TX、PA3=RX、115200 8N1。

与 ESP32 的帧格式唯一依据是 [L431PM_ESP32比赛状态协议.md](docs/L431PM_ESP32比赛状态协议.md)。L431PM 是比赛数据的权威端：负责维护 HP、存活状态、比赛开始/结束状态、热量、功率和射击许可，并通过 USART2 主动发送给 ESP32。ESP32 不重新计算或覆盖这些比赛字段，只负责无线转发；服务器下发的比赛控制命令也由 ESP32 转发至 L431PM 执行。

装甲板 CAN 的协议唯一依据为 [Armor_Cmzy26_081/Docs/CAN_PROTOCOL.md](../Armor_Cmzy26_081/Docs/CAN_PROTOCOL.md)。不要在本工程重复定义装甲板 HIT 包，也不要把 ESP32 的 Wi-Fi 事务号透传到 CAN。

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
- CAN1：`500 kbps`，枪管协议见 [枪管CAN通信协议](docs/枪管CAN通信协议.md)；枪管和四块装甲板的 CAN 业务已在固件中启用，实际总线接线、终端电阻和设备上电状态须上车确认
- USART2：与 ESP32-S3 的比赛状态链路见 [L431PM_ESP32比赛状态协议](docs/L431PM_ESP32比赛状态协议.md)。

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

CAN 已启用：正式固件启动时先为四块装甲板自动分配 NodeID，完成后每 `100 ms` 查询枪管，并以 `25 ms` 轮询间隔查询一块装甲板（每块均为 10 Hz）。三类节点统一为 `500 kbps / 87.5%` 采样点。若任何已分配装甲板持续离线超过恢复宽限期，电管自动重启枚举；同一 UID 的历史受击去重状态会保留，装甲板重启帧或新 UID 才会清除该去重状态。单板或部分装甲板联调时，可仅在测试固件中把 `Application/Referee/app_armor_enum.h` 的 `APP_ARMOR_ENUM_REQUIRED_NODE_COUNT` 编译宏设为 `1~3`；默认值为 `4`，正式比赛版本不得降低。该值同时控制枚举完成门槛和业务状态查询范围。调试时可直接查看 `CAN_RX_COUNT`、`CAN_TX_COUNT`、`CAN_RX_DROP_COUNT`、`CAN_ERROR_COUNT`、`CAN_BUS_OFF_COUNT`、`armor_enum_diag` 与 `referee_can_monitor.armor_nodes`。

## 串口和工具

串口命令见 [电管串口通信命令](docs/电管串口通信命令.md)。LPUART1 上的 `STA=ON` 只控制电源 ASCII 状态输出；比赛业务链路使用 USART2，`APP_Match_Task()` 正常运行时固定每 100 ms 发送一次 12 字节二进制状态帧（含枪管与 NodeID 1~4 装甲板在线位图），并按事件产生 4 字节事件帧。USART2 不使用旧 ASCII 命令格式。LPUART1 会向主控输出比赛射击许可 `FIRE=ON/OFF`：仅由比赛开始/结束、存活和第三张黄牌判负决定，与底盘通断电无关；主控负责最终执行逻辑。发送 `PING` 返回 `PONG` 属于 LPUART1 调试链路。

LPUART1（PC0/PC1）和 USART2（PA2/PA3）均为 `115200 8N1`。

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
