#include "app_referee_can.h"
#include "app_armor_enum.h"
#include "bsp_can.h"
#include "bsp_time.h"
#include <stddef.h>
#include <string.h>

#define APP_REFEREE_CAN_GUN_QUERY_ID 0x231U
#define APP_REFEREE_CAN_GUN_QUERY_PERIOD_MS 100U
#define APP_REFEREE_CAN_GUN_QUERY_FIRST_MS 110U
#define APP_REFEREE_CAN_ARMOR_QUERY_PERIOD_MS 25U
#define APP_REFEREE_CAN_GUN_HEAT_COMMAND_ID 0x233U
#define APP_REFEREE_CAN_GUN_HEAT_COMMAND 0x03U
#define APP_REFEREE_CAN_STRONG_FAULT_ID 0x210U
#define APP_REFEREE_CAN_WEAK_FAULT_ID 0x211U
#define APP_REFEREE_CAN_BOOT_ID 0x212U
#define APP_REFEREE_CAN_CALIBRATION_ACK_ID 0x221U
#define APP_REFEREE_CAN_SHOT_EVENT_ID 0x230U
#define APP_REFEREE_CAN_STATUS_ID 0x232U
#define APP_REFEREE_CAN_CONTROL_ACK_ID 0x234U
#define APP_ARMOR_OFFSET_RESET_CAUSE 0u
#define APP_ARMOR_OFFSET_FAULT_STATUS 1u
#define APP_ARMOR_OFFSET_INIT_DONE 2u
#define APP_ARMOR_OFFSET_HIT_EVENT 3u
#define APP_ARMOR_OFFSET_STATUS_QUERY 4u
#define APP_ARMOR_OFFSET_STATUS_REPLY 5u
#define APP_ARMOR_OFFSET_CONTROL_ACK 7u
#define APP_ARMOR_OFFSET_NODE_ID_QUERY 8u
#define APP_ARMOR_OFFSET_NODE_ID_REPLY 9u
#define APP_ARMOR_OFFSET_HIT_ACK 10u
#define APP_ARMOR_ID_QUERY_TIMEOUT_MS 100u
/* A board is polled every 100 ms.  Do not let one delayed reply use the same
 * 300 ms threshold as its LED communication-lost indicator: that used to
 * trigger a full re-enumeration which itself stopped every board's polling. */
#define APP_REFEREE_CAN_NODE_OFFLINE_MS 1000u
#define APP_REFEREE_CAN_ARMOR_REENUM_GRACE_MS 1000u
#define APP_REFEREE_CAN_GUN_CONTROL_RETRY_MS 100u
#define APP_REFEREE_CAN_GUN_CONTROL_BACKOFF_MS 1000u

APP_RefereeCanMonitor_t referee_can_monitor;
static volatile uint32_t referee_elapsed_ms;
static uint32_t referee_next_gun_query_ms;
static uint32_t referee_last_armor_query_ms;
static uint8_t referee_armor_query_node;
static APP_ArmorIdQuery_t referee_armor_id_query;
static uint8_t referee_armor_enum_was_ready;
static uint32_t referee_armor_enum_ready_since_ms;
typedef struct
{
  uint8_t pending;
  uint8_t heat;
  uint8_t attempts;
  uint32_t next_attempt_ms;
} APP_GunHeatSync_t;
static APP_GunHeatSync_t referee_gun_heat_sync;

static void APP_RefereeCan_Tick1ms(void) { referee_elapsed_ms++; }
static uint16_t APP_RefereeCan_ReadLe16(const uint8_t *d) { return (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8U)); }
static uint32_t APP_RefereeCan_ReadLe32(const uint8_t *d) { return (uint32_t)d[0] | ((uint32_t)d[1] << 8U) | ((uint32_t)d[2] << 16U) | ((uint32_t)d[3] << 24U); }
static uint32_t APP_RefereeCan_ReadLe24(const uint8_t *d) { return (uint32_t)d[0] | ((uint32_t)d[1] << 8U) | ((uint32_t)d[2] << 16U); }

static void APP_RefereeCan_SendHitAck(uint8_t node_id, uint8_t sequence)
{
  uint8_t data = sequence;
  uint16_t id = APP_ArmorEnum_BusinessId(node_id, APP_ARMOR_OFFSET_HIT_ACK);
  if (id != 0U) (void)BSP_Can_Send(id, &data, 1U);
}

static void APP_RefereeCan_SyncArmorNodes(void)
{
  APP_ArmorNodeMonitor_t synchronized[APP_ARMOR_ENUM_NODE_COUNT] = {0};
  uint8_t i;
  uint8_t j;
  referee_can_monitor.armor_enum_ready = APP_ArmorEnum_IsReady();
  /* Keep the old monitor while a new round is collecting. It preserves HIT
   * de-duplication for boards that did not reset; the new mapping is adopted
   * atomically only once all four assignment ACKs have arrived. */
  if (referee_can_monitor.armor_enum_ready == 0U) return;
  for (i = 0u; i < APP_ARMOR_ENUM_NODE_COUNT; i++)
  {
    for (j = 0u; j < APP_ARMOR_ENUM_NODE_COUNT; j++)
    {
      if (referee_can_monitor.armor_nodes[j].node_id != 0U &&
          memcmp(referee_can_monitor.armor_nodes[j].uid, armor_enum_diag.nodes[i].uid,
                 APP_ARMOR_ENUM_UID_BYTES) == 0)
      {
        synchronized[i] = referee_can_monitor.armor_nodes[j];
        break;
      }
    }
    memcpy(synchronized[i].uid, armor_enum_diag.nodes[i].uid, APP_ARMOR_ENUM_UID_BYTES);
    synchronized[i].node_id = armor_enum_diag.nodes[i].node_id;
  }
  memcpy(referee_can_monitor.armor_nodes, synchronized, sizeof(synchronized));
}

static void APP_RefereeCan_UpdateOnlineState(uint32_t now)
{
  uint8_t i;
  if (referee_can_monitor.gun_online != 0U &&
      (uint32_t)(now - referee_can_monitor.last_rx_tick) > APP_REFEREE_CAN_NODE_OFFLINE_MS)
  {
    referee_can_monitor.gun_online = 0U;
  }
  if (APP_ArmorEnum_IsReady() == 0U) return;
  for (i = 0U; i < APP_ARMOR_ENUM_NODE_COUNT; i++)
  {
    APP_ArmorNodeMonitor_t *m = &referee_can_monitor.armor_nodes[i];
    if (m->online != 0U &&
        (uint32_t)(now - m->last_rx_tick) > APP_REFEREE_CAN_NODE_OFFLINE_MS)
    {
      m->online = 0U;
    }
  }
}

static uint8_t APP_RefereeCan_ShouldRestartArmorEnumeration(uint32_t now)
{
  uint8_t i;
  if (APP_ArmorEnum_IsReady() == 0U)
  {
    referee_armor_enum_was_ready = 0U;
    return 0U;
  }
  if (referee_armor_enum_was_ready == 0U)
  {
    referee_armor_enum_was_ready = 1U;
    referee_armor_enum_ready_since_ms = now;
    return 0U;
  }
  if ((uint32_t)(now - referee_armor_enum_ready_since_ms) < APP_REFEREE_CAN_ARMOR_REENUM_GRACE_MS)
  {
    return 0U;
  }
  for (i = 0U; i < APP_ARMOR_ENUM_NODE_COUNT; i++)
  {
    if (referee_can_monitor.armor_nodes[i].node_id != 0U &&
        referee_can_monitor.armor_nodes[i].online == 0U)
    {
      referee_can_monitor.armor_reenum_request_count++;
      referee_can_monitor.armor_reenum_last_ms = now;
      referee_can_monitor.armor_reenum_last_offline_node = (uint8_t)(i + 1U);
      return 1U;
    }
  }
  return 0U;
}

static void APP_RefereeCan_ProcessGunHeatSync(uint32_t now)
{
  uint8_t data[8] = {0U};
  uint32_t retry_ms;
  if (referee_gun_heat_sync.pending == 0U ||
      (int32_t)(now - referee_gun_heat_sync.next_attempt_ms) < 0)
  {
    return;
  }
  data[0] = APP_REFEREE_CAN_GUN_HEAT_COMMAND;
  data[1] = referee_gun_heat_sync.heat;
  (void)BSP_Can_Send(APP_REFEREE_CAN_GUN_HEAT_COMMAND_ID, data, sizeof(data));
  if (referee_gun_heat_sync.attempts < 255U) referee_gun_heat_sync.attempts++;
  retry_ms = (referee_gun_heat_sync.attempts < 3U) ?
             APP_REFEREE_CAN_GUN_CONTROL_RETRY_MS : APP_REFEREE_CAN_GUN_CONTROL_BACKOFF_MS;
  referee_gun_heat_sync.next_attempt_ms = now + retry_ms;
}

static uint8_t APP_RefereeCan_HandleArmorFrame(const BSP_CanFrame_t *frame, uint32_t rx_tick)
{
  int8_t node;
  APP_ArmorNodeMonitor_t *m;
  /* Keep the previous NodeID mapping usable while a replacement enumeration
   * is collecting. This lets status replies refresh liveness without adopting
   * a partially collected mapping. */
  if (APP_ArmorEnum_ActiveNodeCount() == 0U) return 0U;
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_RESET_CAUSE);
  if (node >= 1 && frame->dlc == 1U) { m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)]; m->reset_cause = frame->data[0]; m->hit_sequence_valid = 0U; m->online = 1U; m->last_rx_tick = rx_tick; return 1U; }
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_FAULT_STATUS);
  if (node >= 1 && frame->dlc == 2U) { m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)]; m->fault_flags = APP_RefereeCan_ReadLe16(frame->data); m->online = 1U; m->last_rx_tick = rx_tick; return 1U; }
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_INIT_DONE);
  if (node >= 1 && frame->dlc == 0U) { m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)]; m->boot_received = 1U; m->hit_sequence_valid = 0U; m->online = 1U; m->last_rx_tick = rx_tick; return 1U; }
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_HIT_EVENT);
  if (node >= 1 && frame->dlc == 8U)
  {
    m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)];
    m->online = 1U; m->last_rx_tick = rx_tick;
    /* HIT v2: seq, channel, intensity, force[2], peak[3].  A repeated
     * sequence is acknowledged again but must never deduct HP twice. */
    if (m->hit_sequence_valid == 0U || m->last_hit_sequence != frame->data[0])
    {
      m->last_hit_sequence = frame->data[0]; m->hit_sequence_valid = 1U;
      m->last_hit_channel = frame->data[1]; m->last_hit_intensity = frame->data[2];
      m->last_hit_force_01n = APP_RefereeCan_ReadLe16(&frame->data[3]);
      m->last_hit_sum_peak = APP_RefereeCan_ReadLe24(&frame->data[5]);
      m->hit_event_count++;
    }
    APP_RefereeCan_SendHitAck((uint8_t)node, frame->data[0]);
    return 1U;
  }
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_STATUS_REPLY);
  if (node >= 1 && frame->dlc == 8U) { m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)]; m->state = frame->data[0]; m->reset_cause = frame->data[1]; m->fault_flags = APP_RefereeCan_ReadLe16(&frame->data[2]); m->temperature_fault_flags = APP_RefereeCan_ReadLe16(&frame->data[4]); m->can_health = frame->data[6]; m->online = 1U; m->last_rx_tick = rx_tick; return 1U; }
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_CONTROL_ACK);
  if (node >= 1 && frame->dlc == 8U) { m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)]; m->control_ack_command = frame->data[0]; m->control_ack_parameter = frame->data[1]; m->control_ack_received = 1U; m->online = 1U; m->last_rx_tick = rx_tick; return 1U; }
  node = APP_ArmorEnum_NodeFromBusinessId(frame->standard_id, APP_ARMOR_OFFSET_NODE_ID_REPLY);
  if (node >= 1 && frame->dlc == 8U)
  {
    uint32_t token = APP_RefereeCan_ReadLe32(frame->data) >> 8U;
    uint16_t crc = APP_RefereeCan_ReadLe16(&frame->data[4]);
    m = &referee_can_monitor.armor_nodes[(uint8_t)(node - 1)];
    m->online = 1U; m->last_rx_tick = rx_tick;
    if (referee_armor_id_query.status == APP_ARMOR_ID_QUERY_PENDING &&
        referee_armor_id_query.requested_node_id == (uint8_t)node)
    {
      referee_armor_id_query.reply_node_id = frame->data[0];
      referee_armor_id_query.reply_token = token;
      referee_armor_id_query.reply_uid_crc16 = crc;
      if (frame->data[0] == (uint8_t)node &&
          token == armor_enum_diag.nodes[(uint8_t)(node - 1)].token &&
          crc == armor_enum_diag.nodes[(uint8_t)(node - 1)].uid_crc16)
      {
        referee_armor_id_query.status = APP_ARMOR_ID_QUERY_OK;
      }
      else
      {
        referee_armor_id_query.status = APP_ARMOR_ID_QUERY_MISMATCH;
      }
    }
    return 1U;
  }
  return 0U;
}

static void APP_RefereeCan_HandleFrame(const BSP_CanFrame_t *frame, uint32_t rx_tick)
{
  uint8_t valid = 0U;
  if (frame == NULL) return;
  if (APP_ArmorEnum_OnFrame(frame, referee_elapsed_ms) != 0U) return;
  if (APP_RefereeCan_HandleArmorFrame(frame, rx_tick) != 0U) { referee_can_monitor.received_frame_count++; return; }
  switch (frame->standard_id)
  {
    case APP_REFEREE_CAN_STRONG_FAULT_ID: if (frame->dlc == 1U) { referee_can_monitor.strong_fault_mask = frame->data[0]; valid = 1U; } break;
    case APP_REFEREE_CAN_WEAK_FAULT_ID: if (frame->dlc == 1U) { referee_can_monitor.weak_fault_mask = frame->data[0]; valid = 1U; } break;
    case APP_REFEREE_CAN_BOOT_ID: if (frame->dlc == 0U) { referee_can_monitor.boot_received = 1U; valid = 1U; } break;
    case APP_REFEREE_CAN_CALIBRATION_ACK_ID: if (frame->dlc == 8U) { referee_can_monitor.calibration_front_before = APP_RefereeCan_ReadLe16(&frame->data[0]); referee_can_monitor.calibration_front_after = APP_RefereeCan_ReadLe16(&frame->data[2]); referee_can_monitor.calibration_rear_before = APP_RefereeCan_ReadLe16(&frame->data[4]); referee_can_monitor.calibration_rear_after = APP_RefereeCan_ReadLe16(&frame->data[6]); referee_can_monitor.calibration_ack_received = 1U; valid = 1U; } break;
    case APP_REFEREE_CAN_SHOT_EVENT_ID:
    case APP_REFEREE_CAN_STATUS_ID: if (frame->dlc == 8U) { uint32_t reported_shot_count = APP_RefereeCan_ReadLe32(&frame->data[0]); if (frame->standard_id == APP_REFEREE_CAN_SHOT_EVENT_ID && reported_shot_count > referee_can_monitor.shot_count) referee_can_monitor.shot_event_count++; referee_can_monitor.shot_count = reported_shot_count; referee_can_monitor.last_speed_centimeter_per_second = APP_RefereeCan_ReadLe16(&frame->data[4]); referee_can_monitor.barrel_mask = frame->data[6] & 0x1FU; referee_can_monitor.heat_level = frame->data[7]; valid = 1U; } break;
    case APP_REFEREE_CAN_CONTROL_ACK_ID: if (frame->dlc == 8U) { referee_can_monitor.control_ack_command = frame->data[0]; referee_can_monitor.control_ack_parameter = frame->data[1]; referee_can_monitor.control_ack_received = 1U; if (referee_gun_heat_sync.pending != 0U && frame->data[0] == APP_REFEREE_CAN_GUN_HEAT_COMMAND && frame->data[1] == referee_gun_heat_sync.heat) { referee_gun_heat_sync.pending = 0U; referee_can_monitor.heat_level = referee_gun_heat_sync.heat; } valid = 1U; } break;
    default: break;
  }
  if (valid == 0U) referee_can_monitor.invalid_frame_count++; else { referee_can_monitor.received_frame_count++; referee_can_monitor.last_rx_tick = rx_tick; referee_can_monitor.gun_online = 1U; }
}

void APP_RefereeCan_Init(void)
{
  referee_can_monitor = (APP_RefereeCanMonitor_t){0}; referee_armor_id_query = (APP_ArmorIdQuery_t){0}; referee_gun_heat_sync = (APP_GunHeatSync_t){0}; referee_elapsed_ms = 0U; referee_next_gun_query_ms = APP_REFEREE_CAN_GUN_QUERY_FIRST_MS; referee_last_armor_query_ms = 0U; referee_armor_query_node = 1U; referee_armor_enum_was_ready = 0U; referee_armor_enum_ready_since_ms = 0U;
  (void)BSP_Can_Init(); (void)BSP_Time_Register1msCallback(APP_RefereeCan_Tick1ms); APP_ArmorEnum_Init(0U);
}

void APP_RefereeCan_Task(void)
{
  BSP_CanFrame_t frame; uint32_t now;
  BSP_Can_Task();
  now = referee_elapsed_ms;
  while (BSP_Can_Read(&frame) != 0U) APP_RefereeCan_HandleFrame(&frame, now);
  APP_ArmorEnum_Task(now); APP_RefereeCan_SyncArmorNodes();
  APP_RefereeCan_UpdateOnlineState(now);
  if (APP_RefereeCan_ShouldRestartArmorEnumeration(now) != 0U)
  {
    APP_ArmorEnum_ForceRestart(now);
    referee_armor_id_query = (APP_ArmorIdQuery_t){0};
    referee_armor_query_node = 1U;
    APP_RefereeCan_SyncArmorNodes();
  }
  if (referee_armor_id_query.status == APP_ARMOR_ID_QUERY_PENDING &&
      (int32_t)(now - referee_armor_id_query.deadline_ms) >= 0)
  {
    referee_armor_id_query.status = APP_ARMOR_ID_QUERY_TIMEOUT;
  }
  if ((int32_t)(now - referee_next_gun_query_ms) >= 0)
  {
    referee_next_gun_query_ms += APP_REFEREE_CAN_GUN_QUERY_PERIOD_MS;
    if (BSP_Can_Send(APP_REFEREE_CAN_GUN_QUERY_ID, NULL, 0U) != 0U) { referee_can_monitor.query_tx_count++; referee_can_monitor.gun_query_tx_count++; } else { referee_can_monitor.query_tx_error_count++; referee_can_monitor.gun_query_tx_error_count++; }
  }
  if (APP_ArmorEnum_ActiveNodeCount() != 0U && (uint32_t)(now - referee_last_armor_query_ms) >= APP_REFEREE_CAN_ARMOR_QUERY_PERIOD_MS)
  {
    uint16_t id; referee_last_armor_query_ms = now; id = APP_ArmorEnum_BusinessId(referee_armor_query_node, APP_ARMOR_OFFSET_STATUS_QUERY);
    if (BSP_Can_Send(id, NULL, 0U) != 0U) { referee_can_monitor.query_tx_count++; referee_can_monitor.armor_query_tx_count++; } else { referee_can_monitor.query_tx_error_count++; referee_can_monitor.armor_query_tx_error_count++; }
    referee_armor_query_node++; if (referee_armor_query_node > APP_ArmorEnum_ActiveNodeCount()) referee_armor_query_node = 1U;
  }
  APP_RefereeCan_ProcessGunHeatSync(now);
}

void APP_RefereeCan_RequestGunHeat(uint8_t heat)
{
  referee_gun_heat_sync.pending = 1U;
  referee_gun_heat_sync.heat = heat;
  referee_gun_heat_sync.attempts = 0U;
  referee_gun_heat_sync.next_attempt_ms = referee_elapsed_ms;
}

APP_ArmorIdQueryStatus_t APP_RefereeCan_RequestArmorIdentity(uint8_t node_id)
{
  uint16_t id;
  if (node_id == 0U || node_id > APP_ARMOR_ENUM_REQUIRED_NODE_COUNT)
  {
    referee_armor_id_query.status = APP_ARMOR_ID_QUERY_INVALID_NODE;
    return referee_armor_id_query.status;
  }
  if (APP_ArmorEnum_IsReady() == 0U)
  {
    referee_armor_id_query.status = APP_ARMOR_ID_QUERY_ENUM_NOT_READY;
    return referee_armor_id_query.status;
  }
  if (referee_armor_id_query.status == APP_ARMOR_ID_QUERY_PENDING)
  {
    return APP_ARMOR_ID_QUERY_BUSY;
  }

  referee_armor_id_query = (APP_ArmorIdQuery_t){0};
  referee_armor_id_query.requested_node_id = node_id;
  id = APP_ArmorEnum_BusinessId(node_id, APP_ARMOR_OFFSET_NODE_ID_QUERY);
  if (id == 0U || BSP_Can_Send(id, NULL, 0U) == 0U)
  {
    referee_armor_id_query.status = APP_ARMOR_ID_QUERY_TX_FAILED;
    return referee_armor_id_query.status;
  }
  referee_armor_id_query.status = APP_ARMOR_ID_QUERY_PENDING;
  referee_armor_id_query.deadline_ms = referee_elapsed_ms + APP_ARMOR_ID_QUERY_TIMEOUT_MS;
  return referee_armor_id_query.status;
}

void APP_RefereeCan_GetArmorIdentityQuery(APP_ArmorIdQuery_t *query)
{
  if (query != NULL) { *query = referee_armor_id_query; }
}
