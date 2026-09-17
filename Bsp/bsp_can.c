#include "bsp_can.h"
#include "can.h"
#include <stddef.h>

#define BSP_CAN_RX_QUEUE_DEPTH 32U
#define BSP_CAN_RECOVERY_DELAY_MS 1000U
#define BSP_CAN_TX_TIMEOUT_MS 5U          /* 枚举突发期间容许正常仲裁延迟，超时仍会主动释放邮箱 */
#define BSP_CAN_STUCK_RESTART_MS 100U     /* 三个邮箱全满超过该时长 → 强制重启控制器 */

static BSP_CanFrame_t can_rx_queue[BSP_CAN_RX_QUEUE_DEPTH];
static volatile uint8_t can_rx_head;
static volatile uint8_t can_rx_tail;
static volatile uint8_t can_recovery_pending;
static volatile uint32_t can_recovery_due_tick;

BSP_CanDiagnostics_t bsp_can_diagnostics;
volatile uint32_t CAN_RX_COUNT;
volatile uint32_t CAN_RX_DROP_COUNT;
volatile uint32_t CAN_TX_COUNT;
volatile uint32_t CAN_TX_ERROR_COUNT;
volatile uint32_t CAN_ERROR_COUNT;
volatile uint32_t CAN_LAST_ERROR_CODE;
volatile uint32_t CAN_BUS_OFF_COUNT;
volatile uint32_t CAN_RECOVERY_COUNT;
volatile uint8_t CAN_READY;

/* ==================== 死前追踪：放在 .noinit 段（启动不清零，跨复位保留） ====================
 * 用法：不插 SWD 跑到它不出帧 → 再插 SWD（哪怕 attach 复位了也没关系）→ Watch 里看 g_can_trace
 *   g_can_trace.tx_log[32]：最近 32 次发送，最新一条 = (tx_head-1)%32
 *     .result 1=OK 2=等RQCP超时(5ms) 3=TXOK=0(仲裁/ACK失败) 4=未ready 5=无空闲邮箱
 *   g_can_trace.esr_now：CAN1->ESR  TEC=[23:16] REC=[15:8] LEC=[6:4] BOFF=[2] EPVF=[1]
 *   g_can_trace.tsr_now：CAN1->TSR  TME0/1/2=[26/27/28] 为 1 表示邮箱空闲
 *   g_can_trace.boot_count：每次复位 +1（插 SWD 那次也会 +1）→ 判断是否发生过复位
 * 判据：TEC 涨且 LEC=3 → 无人 ACK；LEC=1/2/4/5 → 物理层位错；TME 不恢复 → 邮箱被占死
 */
#define DBG_TX_LOG_DEPTH 32U
#define DBG_TRACE_MAGIC  0x43414E33u   /* trace layout revision 3: fresh RX-only diagnosis */

typedef struct
{
  uint32_t ms;
  uint16_t id;
  uint8_t  result;
  uint8_t  reserved;
  uint32_t esr;
  uint32_t tsr;
} dbg_tx_log_t;

typedef struct
{
  uint32_t magic;
  uint32_t magic_inv;
  uint32_t boot_count;
  uint32_t tx_head;
  uint32_t tx_attempt;
  uint32_t tx_ok;
  uint32_t tx_timeout;
  uint32_t tx_txok0;
  uint32_t tx_notready;
  uint32_t tx_nomailbox;
  uint32_t esr_now;
  uint32_t tsr_now;
  uint32_t msr_now;
  uint32_t can_state;
  uint32_t error_cb_count;
  uint32_t last_error_code;
  uint32_t boff_count;
  uint32_t recovery_count;
  uint32_t tx_abort_count;       /* 超时后强制 abort 邮箱的次数 */
  uint32_t tx_abort_fail;        /* abort 后邮箱仍未释放的次数（触发控制器重启） */
  uint32_t force_restart_count;  /* 控制器强制重启成功次数 */
  uint32_t first_fault_valid;
  uint32_t first_fault_ms;
  uint16_t first_fault_id;
  uint8_t  first_fault_result;
  uint8_t  first_fault_reserved;
  uint32_t first_fault_esr;
  uint32_t first_fault_tsr;
  uint32_t first_fault_msr;
  uint32_t first_fault_error_code;
  uint32_t first_fault_can_state;
  dbg_tx_log_t tx_log[DBG_TX_LOG_DEPTH];
} bsp_can_trace_t;

__attribute__((section(".noinit"), used)) volatile bsp_can_trace_t g_can_trace;

#define g_dbg_tx_head        (g_can_trace.tx_head)
#define g_dbg_tx_attempt     (g_can_trace.tx_attempt)
#define g_dbg_tx_ok          (g_can_trace.tx_ok)
#define g_dbg_tx_timeout     (g_can_trace.tx_timeout)
#define g_dbg_tx_txok0       (g_can_trace.tx_txok0)
#define g_dbg_tx_notready    (g_can_trace.tx_notready)
#define g_dbg_tx_nomailbox   (g_can_trace.tx_nomailbox)
#define g_dbg_esr_now        (g_can_trace.esr_now)
#define g_dbg_tsr_now        (g_can_trace.tsr_now)
#define g_dbg_msr_now        (g_can_trace.msr_now)
#define g_dbg_can_state      (g_can_trace.can_state)
#define g_dbg_error_cb_count (g_can_trace.error_cb_count)
#define g_dbg_last_error_code (g_can_trace.last_error_code)
#define g_dbg_boff_count     (g_can_trace.boff_count)
#define g_dbg_recovery_count (g_can_trace.recovery_count)

static void Dbg_FreezeFirstFault(uint16_t id, uint8_t result)
{
  if (g_can_trace.first_fault_valid != 0U)
  {
    return;
  }

  g_can_trace.first_fault_ms = HAL_GetTick();
  g_can_trace.first_fault_id = id;
  g_can_trace.first_fault_result = result;
  g_can_trace.first_fault_esr = CAN1->ESR;
  g_can_trace.first_fault_tsr = CAN1->TSR;
  g_can_trace.first_fault_msr = CAN1->MSR;
  g_can_trace.first_fault_error_code = HAL_CAN_GetError(&hcan1);
  g_can_trace.first_fault_can_state = (uint32_t)HAL_CAN_GetState(&hcan1);
  g_can_trace.first_fault_valid = 1U;
}

static void Dbg_TxRecord(uint16_t id, uint8_t result)
{
  uint32_t index = g_can_trace.tx_head % DBG_TX_LOG_DEPTH;

  g_can_trace.tx_log[index].ms = HAL_GetTick();
  g_can_trace.tx_log[index].id = id;
  g_can_trace.tx_log[index].result = result;
  g_can_trace.tx_log[index].reserved = 0U;
  g_can_trace.tx_log[index].esr = CAN1->ESR;
  g_can_trace.tx_log[index].tsr = CAN1->TSR;
  g_can_trace.tx_head = g_can_trace.tx_head + 1U;

  g_can_trace.esr_now = CAN1->ESR;
  g_can_trace.tsr_now = CAN1->TSR;
  g_can_trace.msr_now = CAN1->MSR;
  g_can_trace.can_state = (uint32_t)HAL_CAN_GetState(&hcan1);
}

static uint8_t BSP_Can_Start(void)
{
  uint32_t notifications = CAN_IT_RX_FIFO0_MSG_PENDING |
                           CAN_IT_ERROR_WARNING |
                           CAN_IT_ERROR_PASSIVE |
                           CAN_IT_BUSOFF |
                           CAN_IT_LAST_ERROR_CODE;

  if (HAL_CAN_Start(&hcan1) != HAL_OK)
  {
    return 0U;
  }
  if (HAL_CAN_ActivateNotification(&hcan1, notifications) != HAL_OK)
  {
    (void)HAL_CAN_Stop(&hcan1);
    return 0U;
  }

  bsp_can_diagnostics.ready = 1U;
  CAN_READY = 1U;
  return 1U;
}

uint8_t BSP_Can_Init(void)
{
  CAN_FilterTypeDef filter = {0};

  /* 追踪区在 .noinit：只有魔数校验失败（真上电/首次）才清零，复位不清，
   * 这样"不插 SWD 跑到死 → 再插 SWD"之后仍能读到死前的记录。 */
  if ((g_can_trace.magic != DBG_TRACE_MAGIC) ||
      ((g_can_trace.magic ^ g_can_trace.magic_inv) != 0xFFFFFFFFu))
  {
    volatile uint8_t *p = (volatile uint8_t *)&g_can_trace;
    uint32_t i;
    for (i = 0U; i < sizeof(g_can_trace); i++) { p[i] = 0U; }
    g_can_trace.magic = DBG_TRACE_MAGIC;
    g_can_trace.magic_inv = ~DBG_TRACE_MAGIC;
  }
  g_can_trace.boot_count = g_can_trace.boot_count + 1U;

  can_rx_head = 0U;
  can_rx_tail = 0U;
  can_recovery_pending = 0U;
  can_recovery_due_tick = 0U;
  bsp_can_diagnostics = (BSP_CanDiagnostics_t){0};
  CAN_RX_COUNT = 0U;
  CAN_RX_DROP_COUNT = 0U;
  CAN_TX_COUNT = 0U;
  CAN_TX_ERROR_COUNT = 0U;
  CAN_ERROR_COUNT = 0U;
  CAN_LAST_ERROR_CODE = 0U;
  CAN_BUS_OFF_COUNT = 0U;
  CAN_RECOVERY_COUNT = 0U;
  CAN_READY = 0U;

  filter.FilterBank = 0U;
  filter.FilterMode = CAN_FILTERMODE_IDMASK;
  filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterIdHigh = 0U;
  filter.FilterIdLow = 0U;
  filter.FilterMaskIdHigh = 0U;
  filter.FilterMaskIdLow = 0U;
  filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14U;

  if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK)
  {
    return 0U;
  }

  HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 7U, 0U);
  HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
  HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 7U, 0U);
  HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);

  return BSP_Can_Start();
}

static uint32_t can_mailbox_busy_since;   /* 三个邮箱全满的起始时刻（0=未满） */

static void BSP_Can_ScheduleBusOffRecovery(void)
{
  if (can_recovery_pending != 0U)
  {
    return;
  }

  bsp_can_diagnostics.bus_off_count++;
  CAN_BUS_OFF_COUNT++;
  g_dbg_boff_count = CAN_BUS_OFF_COUNT;
  bsp_can_diagnostics.ready = 0U;
  CAN_READY = 0U;
  can_recovery_pending = 1U;
  can_recovery_due_tick = HAL_GetTick() + BSP_CAN_RECOVERY_DELAY_MS;
}

/* 强制重启 CAN 控制器：abort 所有邮箱 → Stop → Start。
 * 用于兜底"邮箱被僵尸帧占死 / 状态机异常"这类软件无法自行恢复的情况。 */
static void BSP_Can_ForceRestart(void)
{
  Dbg_FreezeFirstFault(0xFFFFU, 7U);
  can_mailbox_busy_since = 0U;
  bsp_can_diagnostics.ready = 0U;
  CAN_READY = 0U;

  (void)HAL_CAN_AbortTxRequest(&hcan1, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
  (void)HAL_CAN_Stop(&hcan1);
  if (BSP_Can_Start() != 0U)
  {
    bsp_can_diagnostics.recovery_count++;
    CAN_RECOVERY_COUNT++;
    g_can_trace.force_restart_count++;
    g_dbg_recovery_count = CAN_RECOVERY_COUNT;
    g_dbg_can_state = (uint32_t)HAL_CAN_GetState(&hcan1);
  }
  else
  {
    can_recovery_pending = 1U;
    can_recovery_due_tick = HAL_GetTick() + BSP_CAN_RECOVERY_DELAY_MS;
  }
}

void BSP_Can_Task(void)
{
  /* 1) bus-off 后的延迟恢复 */
  if (can_recovery_pending != 0U)
  {
    if ((int32_t)(HAL_GetTick() - can_recovery_due_tick) >= 0)
    {
      can_recovery_pending = 0U;
      BSP_Can_ForceRestart();
    }
    return;
  }

  /* Error callbacks are not guaranteed to fire for every bxCAN error path.
   * ESR is the authoritative Bus-Off indication. */
  if ((CAN1->ESR & CAN_ESR_BOFF) != 0U)
  {
    BSP_Can_ScheduleBusOffRecovery();
    return;
  }

  if (bsp_can_diagnostics.ready == 0U)
  {
    return;
  }

  /* 2) 三个发送邮箱全被占住超过 100ms → 僵尸帧占死，强制重启 */
  if ((CAN1->TSR & (CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2)) == 0U)
  {
    if (can_mailbox_busy_since == 0U)
    {
      can_mailbox_busy_since = HAL_GetTick();
    }
    else if ((uint32_t)(HAL_GetTick() - can_mailbox_busy_since) >= BSP_CAN_STUCK_RESTART_MS)
    {
      BSP_Can_ForceRestart();
      return;
    }
  }
  else
  {
    can_mailbox_busy_since = 0U;
  }

  /* 3) 控制器状态异常（不是 LISTENING 或处于初始化态）→ 强制重启。
   * 单帧无 ACK、仲裁延迟或短时线束干扰不是重启整个 CAN 控制器的理由；
   * 上层协议会按自己的节拍重试。 */
  if ((HAL_CAN_GetState(&hcan1) != HAL_CAN_STATE_LISTENING) ||
      ((CAN1->MSR & CAN_MSR_INAK) != 0U))
  {
    BSP_Can_ForceRestart();
  }
}

uint8_t BSP_Can_Read(BSP_CanFrame_t *frame)
{
  uint32_t primask;

  if (frame == NULL)
  {
    return 0U;
  }

  primask = __get_PRIMASK();
  __disable_irq();
  if (can_rx_head == can_rx_tail)
  {
    __set_PRIMASK(primask);
    return 0U;
  }
  *frame = can_rx_queue[can_rx_tail];
  can_rx_tail = (uint8_t)((can_rx_tail + 1U) % BSP_CAN_RX_QUEUE_DEPTH);
  __set_PRIMASK(primask);
  return 1U;
}

uint8_t BSP_Can_Send(uint16_t standard_id, const uint8_t *data, uint8_t dlc)
{
  CAN_TxHeaderTypeDef header = {0};
  uint8_t empty_data[8] = {0};
  uint32_t mailbox;
  uint32_t rqcp;
  uint32_t txok;
  uint32_t start_tick;

  g_dbg_tx_attempt++;

  if ((standard_id > 0x7FFU) || (dlc > 8U) ||
      ((dlc != 0U) && (data == NULL)) ||
      (bsp_can_diagnostics.ready == 0U))
  {
    Dbg_FreezeFirstFault(standard_id, 4U);
    g_dbg_tx_notready++;
    Dbg_TxRecord(standard_id, 4U);
    bsp_can_diagnostics.tx_error_count++;
    CAN_TX_ERROR_COUNT++;
    return 0U;
  }

  header.StdId = standard_id;
  header.IDE = CAN_ID_STD;
  header.RTR = CAN_RTR_DATA;
  header.DLC = dlc;
  header.TransmitGlobalTime = DISABLE;
  if (HAL_CAN_AddTxMessage(&hcan1, &header,
                            (uint8_t *)((data != NULL) ? data : empty_data),
                            &mailbox) != HAL_OK)
  {
    Dbg_FreezeFirstFault(standard_id, 5U);
    g_dbg_tx_nomailbox++;
    Dbg_TxRecord(standard_id, 5U);
    bsp_can_diagnostics.tx_error_count++;
    CAN_TX_ERROR_COUNT++;
    return 0U;                 /* 邮箱占用由 BSP_Can_Task 监控，超时强制重启 */
  }

  if (mailbox == CAN_TX_MAILBOX0) { rqcp = CAN_TSR_RQCP0; txok = CAN_TSR_TXOK0; }
  else if (mailbox == CAN_TX_MAILBOX1) { rqcp = CAN_TSR_RQCP1; txok = CAN_TSR_TXOK1; }
  else if (mailbox == CAN_TX_MAILBOX2) { rqcp = CAN_TSR_RQCP2; txok = CAN_TSR_TXOK2; }
  else
  {
    Dbg_FreezeFirstFault(standard_id, 5U);
    g_dbg_tx_nomailbox++;
    Dbg_TxRecord(standard_id, 5U);
    bsp_can_diagnostics.tx_error_count++; CAN_TX_ERROR_COUNT++;
    return 0U;
  }

  /* NART is enabled: arbitration loss or a missing ACK completes with
   * RQCP set and TXOK clear, so the caller can retry by protocol policy.
   * 关键修复：超时分支必须 abort 邮箱，否则该邮箱被永久占住，三个邮箱占满后
   * HAL_CAN_AddTxMessage 恒返回 HAL_ERROR → 整条 CAN 彻底停发。 */
  start_tick = HAL_GetTick();
  while ((hcan1.Instance->TSR & rqcp) == 0U)
  {
    if (((hcan1.Instance->ESR & CAN_ESR_BOFF) != 0U) ||
        ((uint32_t)(HAL_GetTick() - start_tick) >= BSP_CAN_TX_TIMEOUT_MS))
    {
      Dbg_FreezeFirstFault(standard_id, 2U);
      (void)HAL_CAN_AbortTxRequest(&hcan1, mailbox);
      g_can_trace.tx_abort_count++;
      g_dbg_tx_timeout++;
      bsp_can_diagnostics.tx_error_count++;
      CAN_TX_ERROR_COUNT++;
      Dbg_TxRecord(standard_id, 2U);
      if ((hcan1.Instance->TSR & rqcp) == 0U)
      {
        g_can_trace.tx_abort_fail++;        /* abort 也没释放：交给 Task 重启控制器 */
        Dbg_TxRecord(standard_id, 6U);
      }
      return 0U;
    }
  }
  if ((hcan1.Instance->TSR & txok) == 0U)
  {
    Dbg_FreezeFirstFault(standard_id, 3U);
    hcan1.Instance->TSR = rqcp;
    g_dbg_tx_txok0++;
    Dbg_TxRecord(standard_id, 3U);
    bsp_can_diagnostics.tx_error_count++;
    CAN_TX_ERROR_COUNT++;
    return 0U;
  }
  hcan1.Instance->TSR = rqcp;

  g_dbg_tx_ok++;
  Dbg_TxRecord(standard_id, 1U);
  bsp_can_diagnostics.tx_count++;
  CAN_TX_COUNT++;
  return 1U;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef header;
  BSP_CanFrame_t frame;
  uint8_t next_head;

  if ((hcan != &hcan1) ||
      (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, frame.data) != HAL_OK))
  {
    return;
  }
  if ((header.IDE != CAN_ID_STD) || (header.RTR != CAN_RTR_DATA) ||
      (header.DLC > 8U))
  {
    return;
  }

  frame.standard_id = (uint16_t)header.StdId;
  frame.dlc = (uint8_t)header.DLC;
  next_head = (uint8_t)((can_rx_head + 1U) % BSP_CAN_RX_QUEUE_DEPTH);
  if (next_head == can_rx_tail)
  {
    bsp_can_diagnostics.rx_drop_count++;
    CAN_RX_DROP_COUNT++;
    return;
  }

  can_rx_queue[can_rx_head] = frame;
  can_rx_head = next_head;
  bsp_can_diagnostics.rx_count++;
  CAN_RX_COUNT++;
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  uint32_t error;

  if (hcan != &hcan1)
  {
    return;
  }

  error = HAL_CAN_GetError(hcan);
  Dbg_FreezeFirstFault(0xFFFFU, 8U);
  bsp_can_diagnostics.error_count++;
  bsp_can_diagnostics.last_error_code = error;
  CAN_ERROR_COUNT++;
  CAN_LAST_ERROR_CODE = error;
  g_dbg_error_cb_count++;
  g_dbg_last_error_code = error;
  g_dbg_esr_now = CAN1->ESR;
  g_dbg_tsr_now = CAN1->TSR;
  g_dbg_msr_now = CAN1->MSR;
  g_dbg_can_state = (uint32_t)HAL_CAN_GetState(&hcan1);
  g_dbg_recovery_count = CAN_RECOVERY_COUNT;
  if ((error & HAL_CAN_ERROR_BOF) != 0U)
  {
    BSP_Can_ScheduleBusOffRecovery();
  }
}
