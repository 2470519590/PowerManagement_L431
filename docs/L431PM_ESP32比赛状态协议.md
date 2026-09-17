# L431PM 与 ESP32-S3 比赛状态协议

USART2：`115200, 8N1`，无硬件流控。L431 为 PA2 TX、PA3 RX；ESP32-S3 为 GPIO18 RX、GPIO17 TX，交叉连接并共地。该链路是二进制协议，不使用 LPUART1 的 ASCII 命令。

CRC 为 CRC-8/ATM：poly `0x07`、init `0x00`、RefIn/RefOut=false、xorout `0x00`。所有多字节数值小端。接收端以连续两字节帧头同步；CRC 失败只丢弃当前候选帧。

## L431 → ESP32

状态帧每 100 ms 自动发送，长度 12 字节：

| 字节 | 内容 |
|---:|---|
| 0~1 | `A5 5A` |
| 2 | sequence |
| 3 | bit0=alive，bit1=shoot_enabled，bit2=power_on（底盘供电输出已打开） |
| 4~5 | HP，`uint16_t` |
| 6~7 | 枪管 heat，`uint16_t` |
| 8~9 | 底盘平均功率 W，`uint16_t` 饱和至 65535 |
| 10 | 设备在线位图：bit0=枪管，bit1~4=装甲板 NodeID 1~4；未完成四板 NodeID 分配时 bit1~4 均为 0 |
| 11 | CRC-8，覆盖 Byte0~10 |

`shoot_enabled` 是比赛射击许可：比赛已开始、机器人存活且未判负时为 1。它与 `power_on` 完全独立；`power_on` 只反映底盘供电输出是否打开。

枪管每 100 ms 被查询一次、每块装甲板每 100 ms 被查询一次。L431 在连续 1000 ms 未收到有效回复后将对应 bit 清零；每块装甲板独立判定。自动枚举期间或四块板未全部获得 NodeID 时，四块装甲板均视为离线，避免未分配节点被当作可正常上报受击。即使 CAN 回复仍在到达，装甲板状态为 `BOOT`、`FAULT`、`COMM_LOST`、`ID_SETUP` 或 `ID_CONFLICT` 时也视为离线；仅 `NORMAL` 和瞬时 `HIT` 状态表示能够正常上报受击。

异步事件均为 4 字节：事件头两字节、sequence、CRC-8。`B1 1B`=进入战斗、`B2 2B`=受击、`B3 3B`=死亡、`B4 4B`=复活、`B5 5B`=允许射击、`B6 6B`=禁止射击、`B7 7B`=脱离战斗。

`B1/B7` 表示战斗状态切换：累计有效射击数增长时仅在未战斗状态发送一次 `B1`；最后一次有效射击后连续 `800 ms` 未再增长时发送一次 `B7`。`B5/B6` 只在比赛射击许可改变时产生，条件与状态帧的 `shoot_enabled` 完全相同，不由底盘通断电产生。受击和射击许可事件为单发；若单发事件丢失，接收端以后续 10 Hz 状态帧为准。死亡、复活为 UART 可靠事件：L431 保留原 `sequence` 重发，直到收到 ESP32 的事件 ACK。首次发送后每 `100 ms` 重发两次，之后每 `1000 ms` 重发；ESP32 只有将对应 UDP 可靠事件写入 NVS 后才回复 ACK。因此 ESP32 重启后，L431 重发同一事件不会创建新的 UDP 事务。

## ESP32 → L431

| 命令 | 帧 | 长度 | 回执 |
|---|---|---:|---|
| 比赛开始 | `C1 1C + transaction_id[4] + CRC` | 7 | `D1 1D + transaction_id[4] + result + CRC` |
| 比赛结束 | `C2 2C + transaction_id[4] + CRC` | 7 | `D2 2D + transaction_id[4] + result + CRC` |
| 设置 HP | `C3 3C + transaction_id[4] + hp[2] + CRC` | 9 | `D3 3D + transaction_id[4] + result + CRC` |
| 黄牌处罚 | `C4 4C + transaction_id[4] + CRC` | 7 | `D4 4D + transaction_id[4] + result + CRC` |
| 强制断电 | `C5 5C + transaction_id[4] + CRC` | 7 | `D5 5D + transaction_id[4] + result + CRC` |
| 请求通电 | `C6 6C + transaction_id[4] + CRC` | 7 | `D6 6D + transaction_id[4] + result + CRC` |

ESP32 对可靠 UART 事件的确认帧同为 4 字节：

| 确认对象 | 帧 |
|---|---|
| 死亡 `B3 3B` | `E3 3E + sequence + CRC` |
| 复活 `B4 4B` | `E4 4E + sequence + CRC` |

`result`：0=成功，1=不允许，2=失败。相同命令和 transaction_id 重复到达时不重复执行，只重发原回执。HP 仅允许 0~300。比赛开始后 L431PM 自行设置 `HP=300、alive=1`，清除本场黄牌和判负状态；比赛结束前，HP 归零后等待 5 秒自动复活至 300。每次有效受击扣 20。黄牌处罚由服务器每次发送一条 `C4` 命令：本场第 1、2 次各扣 50 HP，第 3 次直接判负并不再自动复活。黄牌计数、HP 扣除、复活和判负均由 L431PM 执行，ESP32 只转发命令。

`C5` 只关闭底盘/弹道供电输出，不关闭 L431PM；`C6` 请求恢复底盘供电，是否允许由电源保护条件决定。两条命令都必须返回对应 ACK。状态帧的 `power_on` 由 `power_monitor.switch_chassis` 解算后放入 bit2，ESP32 原样转发到 UDP 状态帧。
