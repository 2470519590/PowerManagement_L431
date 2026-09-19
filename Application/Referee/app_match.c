#include "app_match.h"
#include "app_power.h"
#include "app_referee_can.h"
#include "bsp_uart2.h"
#include <string.h>

#define MATCH_STATUS_MAGIC0 0xA5U
#define MATCH_STATUS_MAGIC1 0x5AU
#define MATCH_STATUS_LENGTH 12U
#define MATCH_EVENT_LENGTH 4U
#define MATCH_STATUS_PERIOD_MS 100U
#define MATCH_REVIVE_DELAY_MS 5000U
#define MATCH_CMD_START0 0xC1U
#define MATCH_CMD_START1 0x1CU
#define MATCH_CMD_END0 0xC2U
#define MATCH_CMD_END1 0x2CU
#define MATCH_CMD_HP0 0xC3U
#define MATCH_CMD_HP1 0x3CU
#define MATCH_CMD_YELLOW0 0xC4U
#define MATCH_CMD_YELLOW1 0x4CU
#define MATCH_CMD_POWER_OFF0 0xC5U
#define MATCH_CMD_POWER_OFF1 0x5CU
#define MATCH_CMD_POWER_ON0 0xC6U
#define MATCH_CMD_POWER_ON1 0x6CU
#define MATCH_ACK_START0 0xD1U
#define MATCH_ACK_START1 0x1DU
#define MATCH_ACK_END0 0xD2U
#define MATCH_ACK_END1 0x2DU
#define MATCH_ACK_HP0 0xD3U
#define MATCH_ACK_HP1 0x3DU
#define MATCH_ACK_YELLOW0 0xD4U
#define MATCH_ACK_YELLOW1 0x4DU
#define MATCH_ACK_POWER_OFF0 0xD5U
#define MATCH_ACK_POWER_OFF1 0x5DU
#define MATCH_ACK_POWER_ON0 0xD6U
#define MATCH_ACK_POWER_ON1 0x6DU
#define MATCH_EVENT_ATTACK0 0xB1U
#define MATCH_EVENT_ATTACK1 0x1BU
#define MATCH_EVENT_HIT0 0xB2U
#define MATCH_EVENT_HIT1 0x2BU
#define MATCH_EVENT_DEAD0 0xB3U
#define MATCH_EVENT_DEAD1 0x3BU
#define MATCH_EVENT_REVIVE0 0xB4U
#define MATCH_EVENT_REVIVE1 0x4BU
#define MATCH_EVENT_SHOOT_ON0 0xB5U
#define MATCH_EVENT_SHOOT_ON1 0x5BU
#define MATCH_EVENT_SHOOT_OFF0 0xB6U
#define MATCH_EVENT_SHOOT_OFF1 0x6BU
#define MATCH_EVENT_COMBAT_END0 0xB7U
#define MATCH_EVENT_COMBAT_END1 0x7BU
#define MATCH_EVENT_ACK_DEAD0 0xE3U
#define MATCH_EVENT_ACK_DEAD1 0x3EU
#define MATCH_EVENT_ACK_REVIVE0 0xE4U
#define MATCH_EVENT_ACK_REVIVE1 0x4EU
/* The controller can be exercised without a server.  It therefore boots in
 * an active preparation round at 200 HP.  A server GAME_START atomically
 * promotes that round to the official 300 HP round. */
#define MATCH_HP_PREPARE_MAX 200U
#define MATCH_HP_MATCH_MAX 300U
#define MATCH_HIT_DAMAGE 20U
#define MATCH_YELLOW_DAMAGE 50U
#define MATCH_YELLOW_FORFEIT_COUNT 3U
#define MATCH_COMBAT_IDLE_MS 800U
#define MATCH_EVENT_QUEUE_DEPTH 8U
#define MATCH_RELIABLE_EVENT_RETRY_MS 100U
#define MATCH_RELIABLE_EVENT_BACKOFF_MS 1000U
#define MATCH_RX_BUFFER_SIZE 16U
#define MATCH_RESULT_OK 0U
#define MATCH_RESULT_DENIED 1U
#define MATCH_RESULT_FAILED 2U
#define MATCH_TRANSACTION_HISTORY_DEPTH 8U
#define MATCH_DEVICE_ONLINE_GUN 0x01U
#define MATCH_DEVICE_ONLINE_ARMOR_NODE1 0x02U
#define MATCH_ARMOR_STATE_NORMAL 1U
#define MATCH_ARMOR_STATE_HIT 2U

typedef struct {
  uint8_t first; uint8_t second; uint8_t sequence; uint8_t reliable;
  uint8_t attempts; uint32_t next_send_ms;
} MatchEvent_t;
typedef struct { uint8_t command; uint32_t transaction_id; uint8_t result; uint8_t valid; } MatchTransaction_t;

static uint16_t match_hp;
static uint16_t match_hp_limit;
static uint8_t match_alive;
static uint8_t match_started;
static uint8_t match_yellow_cards;
static uint8_t match_forfeited;
static uint8_t match_shoot_enabled;
static uint32_t match_shoot_permission_sequence;
static uint32_t match_revive_due_ms;
static uint32_t match_last_status_ms;
static uint8_t match_status_sequence;
static uint8_t match_event_sequence;
static MatchEvent_t match_event_queue[MATCH_EVENT_QUEUE_DEPTH];
static uint8_t match_event_head;
static uint8_t match_event_tail;
static uint32_t match_last_shot_count;
static uint32_t match_last_shot_tick;
static uint8_t match_combat_active;
static uint32_t match_last_hit_count[APP_ARMOR_ENUM_NODE_COUNT];
static uint8_t match_last_hit_uid[APP_ARMOR_ENUM_NODE_COUNT][APP_ARMOR_ENUM_UID_BYTES];
static uint8_t match_rx[MATCH_RX_BUFFER_SIZE];
static uint8_t match_rx_length;
static MatchTransaction_t match_transactions[MATCH_TRANSACTION_HISTORY_DEPTH];
static uint8_t match_transaction_next;

static uint8_t Match_Crc8(const uint8_t *data, uint8_t len)
{
  uint8_t crc = 0U; uint8_t i;
  while (len-- != 0U) { crc ^= *data++; for (i = 0U; i < 8U; i++) crc = ((crc & 0x80U) != 0U) ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U); }
  return crc;
}

static uint32_t Match_ReadLe32(const uint8_t *data) { return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U); }
static uint16_t Match_ReadLe16(const uint8_t *data) { return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U)); }
static uint8_t Match_TimeReached(uint32_t now, uint32_t due) { return (int32_t)(now - due) >= 0 ? 1U : 0U; }
static void Match_QueueEvent(uint8_t first, uint8_t second);

static void Match_StopCombat(void)
{
  if (match_combat_active != 0U)
  {
    match_combat_active = 0U;
    Match_QueueEvent(MATCH_EVENT_COMBAT_END0, MATCH_EVENT_COMBAT_END1);
  }
}

static void Match_UpdateShootPermission(void)
{
  uint8_t enabled = (match_started != 0U && match_alive != 0U &&
                     match_forfeited == 0U) ? 1U : 0U;
  if (match_shoot_enabled != enabled)
  {
    match_shoot_enabled = enabled;
    match_shoot_permission_sequence++;
    Match_QueueEvent(enabled != 0U ? MATCH_EVENT_SHOOT_ON0 : MATCH_EVENT_SHOOT_OFF0,
                     enabled != 0U ? MATCH_EVENT_SHOOT_ON1 : MATCH_EVENT_SHOOT_OFF1);
  }
}

static void Match_QueueEvent(uint8_t first, uint8_t second)
{
  uint8_t next = (uint8_t)((match_event_head + 1U) % MATCH_EVENT_QUEUE_DEPTH);
  uint8_t after_next = (uint8_t)((next + 1U) % MATCH_EVENT_QUEUE_DEPTH);
  uint8_t reliable = (first == MATCH_EVENT_DEAD0 || first == MATCH_EVENT_REVIVE0) ? 1U : 0U;
  /* Preserve one queue slot for a death/revive event. */
  if (next == match_event_tail || (reliable == 0U && after_next == match_event_tail)) return;
  match_event_queue[match_event_head] = (MatchEvent_t){
    .first = first, .second = second, .sequence = match_event_sequence++, .reliable = reliable,
  };
  match_event_head = next;
}

static void Match_SetAlive(uint8_t alive, uint32_t now)
{
  if (alive == 0U)
  {
    Match_StopCombat();
    if (match_alive != 0U) Match_QueueEvent(MATCH_EVENT_DEAD0, MATCH_EVENT_DEAD1);
    match_alive = 0U; match_hp = 0U; match_revive_due_ms = now + MATCH_REVIVE_DELAY_MS;
  }
  else
  {
    if (match_alive == 0U) Match_QueueEvent(MATCH_EVENT_REVIVE0, MATCH_EVENT_REVIVE1);
    match_alive = 1U; match_revive_due_ms = 0U;
  }
  Match_UpdateShootPermission();
}

static void Match_SendAck(uint8_t command, uint32_t transaction_id, uint8_t result)
{
  uint8_t frame[8];
  frame[0] = (uint8_t)(command + 0x10U); frame[1] = (uint8_t)(command == MATCH_CMD_START0 ? MATCH_ACK_START1 : (command == MATCH_CMD_END0 ? MATCH_ACK_END1 : (command == MATCH_CMD_HP0 ? MATCH_ACK_HP1 : (command == MATCH_CMD_YELLOW0 ? MATCH_ACK_YELLOW1 : (command == MATCH_CMD_POWER_OFF0 ? MATCH_ACK_POWER_OFF1 : MATCH_ACK_POWER_ON1)))));
  frame[2] = (uint8_t)transaction_id; frame[3] = (uint8_t)(transaction_id >> 8U); frame[4] = (uint8_t)(transaction_id >> 16U); frame[5] = (uint8_t)(transaction_id >> 24U); frame[6] = result; frame[7] = Match_Crc8(frame, 7U);
  (void)BSP_Uart2_Send(frame, sizeof(frame));
}

static void Match_HandleCommand(const uint8_t *frame, uint8_t length, uint32_t now)
{
  uint8_t command = frame[0]; uint8_t index;
  uint32_t transaction_id = Match_ReadLe32(&frame[2]); uint8_t result = MATCH_RESULT_OK;
  for (index = 0U; index < MATCH_TRANSACTION_HISTORY_DEPTH; index++)
  {
    if (match_transactions[index].valid != 0U && match_transactions[index].command == command && match_transactions[index].transaction_id == transaction_id)
    {
      Match_SendAck(command, transaction_id, match_transactions[index].result);
      return;
    }
  }
  if (command == MATCH_CMD_START0)
  {
    match_started = 1U;
    match_yellow_cards = 0U;
    match_forfeited = 0U;
    match_revive_due_ms = 0U;
    match_hp_limit = MATCH_HP_MATCH_MAX;
    match_hp = match_hp_limit;
    match_alive = 1U;
    match_combat_active = 0U;
    match_last_shot_count = referee_can_monitor.shot_count;
    APP_RefereeCan_RequestGunHeat(0U);
    Match_UpdateShootPermission();
  }
  else if (command == MATCH_CMD_END0) { Match_StopCombat(); match_started = 0U; Match_UpdateShootPermission(); }
  else if (command == MATCH_CMD_HP0)
  {
    uint16_t hp = Match_ReadLe16(&frame[6]);
    if (length != 9U || hp > match_hp_limit) result = MATCH_RESULT_DENIED;
    else if (hp == 0U) Match_SetAlive(0U, now);
    else { match_hp = hp; if (match_alive == 0U) Match_SetAlive(1U, now); }
  }
  else if (command == MATCH_CMD_YELLOW0)
  {
    if (length != 7U || match_started == 0U || match_forfeited != 0U)
      result = MATCH_RESULT_DENIED;
    else
    {
      if (match_yellow_cards < 255U) match_yellow_cards++;
      /* Every yellow card costs 50 HP, including the third card. */
      if (match_alive != 0U)
      {
        match_hp = (match_hp > MATCH_YELLOW_DAMAGE) ?
                   (uint16_t)(match_hp - MATCH_YELLOW_DAMAGE) : 0U;
        if (match_hp == 0U) Match_SetAlive(0U, now);
      }
      if (match_yellow_cards >= MATCH_YELLOW_FORFEIT_COUNT)
      {
        Match_StopCombat();
        if (match_alive != 0U) Match_QueueEvent(MATCH_EVENT_DEAD0, MATCH_EVENT_DEAD1);
        match_forfeited = 1U;
        match_revive_due_ms = 0U;
        match_alive = 0U;
        match_hp = 0U;
        Match_UpdateShootPermission();
        /* A third yellow card loses this robot only.  Keep the match active
         * for the opponent, but cut this robot's chassis output. */
        (void)APP_Power_ForceChassisState(POWER_SWITCH_OFF);
      }
    }
  }
  else if (command == MATCH_CMD_POWER_OFF0)
  {
    if (length != 7U || APP_Power_ForceChassisState(POWER_SWITCH_OFF) == 0U)
      result = MATCH_RESULT_FAILED;
  }
  else if (command == MATCH_CMD_POWER_ON0)
  {
    if (length != 7U || APP_Power_RequestChassisOn() == 0U)
      result = MATCH_RESULT_FAILED;
  }
  else result = MATCH_RESULT_FAILED;
  match_transactions[match_transaction_next].command = command;
  match_transactions[match_transaction_next].transaction_id = transaction_id;
  match_transactions[match_transaction_next].result = result;
  match_transactions[match_transaction_next].valid = 1U;
  match_transaction_next = (uint8_t)((match_transaction_next + 1U) % MATCH_TRANSACTION_HISTORY_DEPTH);
  Match_SendAck(command, transaction_id, result);
}

static void Match_HandleEventAck(const uint8_t *frame)
{
  MatchEvent_t *event;
  if (match_event_tail == match_event_head) return;
  event = &match_event_queue[match_event_tail];
  if (event->reliable == 0U || event->sequence != frame[2]) return;
  if ((frame[0] == MATCH_EVENT_ACK_DEAD0 && event->first != MATCH_EVENT_DEAD0) ||
      (frame[0] == MATCH_EVENT_ACK_REVIVE0 && event->first != MATCH_EVENT_REVIVE0)) return;
  match_event_tail = (uint8_t)((match_event_tail + 1U) % MATCH_EVENT_QUEUE_DEPTH);
}

static void Match_ParseRx(uint32_t now)
{
  uint8_t byte;
  while (BSP_Uart2_ReadByte(&byte) != 0U)
  {
    uint8_t expected = 0U;
    if (match_rx_length < MATCH_RX_BUFFER_SIZE) match_rx[match_rx_length++] = byte; else match_rx_length = 0U;
    while (match_rx_length >= 2U)
    {
      if ((match_rx[0] == MATCH_CMD_START0 && match_rx[1] == MATCH_CMD_START1) || (match_rx[0] == MATCH_CMD_END0 && match_rx[1] == MATCH_CMD_END1)) expected = 7U;
      else if (match_rx[0] == MATCH_CMD_HP0 && match_rx[1] == MATCH_CMD_HP1) expected = 9U;
      else if (match_rx[0] == MATCH_CMD_YELLOW0 && match_rx[1] == MATCH_CMD_YELLOW1) expected = 7U;
      else if ((match_rx[0] == MATCH_CMD_POWER_OFF0 && match_rx[1] == MATCH_CMD_POWER_OFF1) ||
               (match_rx[0] == MATCH_CMD_POWER_ON0 && match_rx[1] == MATCH_CMD_POWER_ON1)) expected = 7U;
      else if ((match_rx[0] == MATCH_EVENT_ACK_DEAD0 && match_rx[1] == MATCH_EVENT_ACK_DEAD1) ||
               (match_rx[0] == MATCH_EVENT_ACK_REVIVE0 && match_rx[1] == MATCH_EVENT_ACK_REVIVE1)) expected = 4U;
      else { memmove(match_rx, &match_rx[1], --match_rx_length); continue; }
      if (match_rx_length < expected) break;
      if (Match_Crc8(match_rx, (uint8_t)(expected - 1U)) == match_rx[expected - 1U])
      {
        if (expected == 4U) Match_HandleEventAck(match_rx);
        else Match_HandleCommand(match_rx, expected, now);
      }
      memmove(match_rx, &match_rx[expected], (size_t)(match_rx_length - expected)); match_rx_length = (uint8_t)(match_rx_length - expected);
    }
  }
}

static void Match_PollCanEvents(uint32_t now)
{
  uint8_t i;
  /* The gun's cumulative count is the authoritative recovery path. A lost
   * 0x230 event is therefore still observed on the next 0x232 poll. */
  if (referee_can_monitor.shot_count < match_last_shot_count)
  {
    match_last_shot_count = referee_can_monitor.shot_count;
  }
  else if (referee_can_monitor.shot_count != match_last_shot_count)
  {
    match_last_shot_count = referee_can_monitor.shot_count;
    if (match_started != 0U && match_forfeited == 0U && match_alive != 0U)
    {
      match_last_shot_tick = now;
      if (match_combat_active == 0U)
      {
        match_combat_active = 1U;
        Match_QueueEvent(MATCH_EVENT_ATTACK0, MATCH_EVENT_ATTACK1);
      }
    }
  }
  if (match_combat_active != 0U &&
      (match_started == 0U || match_forfeited != 0U || match_alive == 0U ||
       (uint32_t)(now - match_last_shot_tick) >= MATCH_COMBAT_IDLE_MS)) Match_StopCombat();
  for (i = 0U; i < APP_ARMOR_ENUM_NODE_COUNT; i++)
  {
    /* A re-enumeration can move a surviving physical board to a different
     * NodeID when another board is replaced. Per-slot counters are therefore
     * meaningful only while the slot still carries the same hardware UID. */
    if (memcmp(match_last_hit_uid[i], referee_can_monitor.armor_nodes[i].uid,
               APP_ARMOR_ENUM_UID_BYTES) != 0)
    {
      memcpy(match_last_hit_uid[i], referee_can_monitor.armor_nodes[i].uid,
             APP_ARMOR_ENUM_UID_BYTES);
      match_last_hit_count[i] = referee_can_monitor.armor_nodes[i].hit_event_count;
      continue;
    }
    if (referee_can_monitor.armor_nodes[i].hit_event_count < match_last_hit_count[i])
    {
      match_last_hit_count[i] = referee_can_monitor.armor_nodes[i].hit_event_count;
      continue;
    }
    while (match_last_hit_count[i] != referee_can_monitor.armor_nodes[i].hit_event_count)
    {
      match_last_hit_count[i]++;
      if (match_started != 0U && match_forfeited == 0U && match_alive != 0U) { Match_QueueEvent(MATCH_EVENT_HIT0, MATCH_EVENT_HIT1); if (match_hp <= MATCH_HIT_DAMAGE) Match_SetAlive(0U, now); else match_hp = (uint16_t)(match_hp - MATCH_HIT_DAMAGE); }
    }
  }
}

static void Match_SendPending(uint32_t now)
{
  uint8_t frame[MATCH_EVENT_LENGTH]; MatchEvent_t *event;
  if (match_event_tail == match_event_head) return;
  event = &match_event_queue[match_event_tail];
  if (event->reliable != 0U && event->attempts != 0U &&
      Match_TimeReached(now, event->next_send_ms) == 0U) return;
  frame[0] = event->first; frame[1] = event->second; frame[2] = event->sequence; frame[3] = Match_Crc8(frame, 3U);
  if (BSP_Uart2_Send(frame, sizeof(frame)) == 0U) return;
  if (event->reliable == 0U)
  {
    match_event_tail = (uint8_t)((match_event_tail + 1U) % MATCH_EVENT_QUEUE_DEPTH);
    return;
  }
  event->attempts++;
  event->next_send_ms = now + (event->attempts < 3U ?
                                MATCH_RELIABLE_EVENT_RETRY_MS : MATCH_RELIABLE_EVENT_BACKOFF_MS);
}

static void Match_SendStatus(uint32_t now)
{
  PowerReportSnapshot_t snapshot; uint32_t sequence; uint8_t frame[MATCH_STATUS_LENGTH]; uint16_t heat = referee_can_monitor.heat_level; uint8_t device_online = 0U; uint8_t i;
  if ((uint32_t)(now - match_last_status_ms) < MATCH_STATUS_PERIOD_MS) return;
  match_last_status_ms = now;
  if (APP_Power_GetLatestReportSnapshot(&snapshot, &sequence) == 0U) snapshot = (PowerReportSnapshot_t){0};
  frame[0] = MATCH_STATUS_MAGIC0; frame[1] = MATCH_STATUS_MAGIC1; frame[2] = match_status_sequence++;
  frame[3] = (uint8_t)((match_alive != 0U ? 0x01U : 0U) |
                       (match_shoot_enabled != 0U ? 0x02U : 0U) |
                       (power_monitor.switch_chassis == POWER_SWITCH_ON ? 0x04U : 0U));
  frame[4] = (uint8_t)match_hp; frame[5] = (uint8_t)(match_hp >> 8U); frame[6] = (uint8_t)heat; frame[7] = (uint8_t)(heat >> 8U);
  frame[8] = (uint8_t)snapshot.chassis_power_avg_w; frame[9] = (uint8_t)(snapshot.chassis_power_avg_w > 65535U ? 0xFFU : snapshot.chassis_power_avg_w >> 8U);
  if (snapshot.chassis_power_avg_w > 65535U) frame[8] = 0xFFU;
  if (referee_can_monitor.gun_online != 0U) device_online |= MATCH_DEVICE_ONLINE_GUN;
  /* Before all four assignments ACK, none of the nodes can yet report hits
   * through a stable slot, so all armor bits intentionally remain offline. */
  if (APP_ArmorEnum_IsReady() != 0U)
  {
    for (i = 0U; i < APP_ARMOR_ENUM_NODE_COUNT; i++)
    {
      /* The new armor firmware only emits a referee hit in NORMAL/HIT.
       * A responding board in BOOT, FAULT, COMM_LOST or ID setup/conflict
       * therefore remains visible to diagnostics but is offline to match. */
      if (referee_can_monitor.armor_nodes[i].online != 0U &&
          (referee_can_monitor.armor_nodes[i].state == MATCH_ARMOR_STATE_NORMAL ||
           referee_can_monitor.armor_nodes[i].state == MATCH_ARMOR_STATE_HIT))
      {
        device_online |= (uint8_t)(MATCH_DEVICE_ONLINE_ARMOR_NODE1 << i);
      }
    }
  }
  frame[10] = device_online;
  frame[11] = Match_Crc8(frame, 11U); (void)BSP_Uart2_Send(frame, sizeof(frame));
}

void APP_Match_Init(void)
{
  uint8_t i;
  /* Default to a fully functional preparation round.  This keeps real
   * armor-hit, death, revival and shoot-permission paths testable while the
   * match server is absent; GAME_START below replaces it with 300 HP. */
  match_hp_limit = MATCH_HP_PREPARE_MAX;
  match_hp = match_hp_limit;
  match_alive = 1U;
  match_started = 1U;
  match_yellow_cards = 0U;
  match_forfeited = 0U;
  match_shoot_enabled = 0U;
  match_shoot_permission_sequence = 1U;
  match_revive_due_ms = 0U;
  match_last_status_ms = 0U;
  match_status_sequence = 0U;
  match_event_sequence = 0U;
  match_event_head = 0U;
  match_event_tail = 0U;
  match_rx_length = 0U;
  memset(match_transactions, 0, sizeof(match_transactions)); match_transaction_next = 0U;
  match_last_shot_count = referee_can_monitor.shot_count;
  match_last_shot_tick = 0U;
  match_combat_active = 0U;
  for (i = 0U; i < APP_ARMOR_ENUM_NODE_COUNT; i++)
  {
    match_last_hit_count[i] = referee_can_monitor.armor_nodes[i].hit_event_count;
    memcpy(match_last_hit_uid[i], referee_can_monitor.armor_nodes[i].uid,
           APP_ARMOR_ENUM_UID_BYTES);
  }
  BSP_Uart2_Init();
  Match_UpdateShootPermission();
}

uint8_t APP_Match_GetShootPermission(uint8_t *enabled, uint32_t *sequence)
{
  if ((enabled == NULL) || (sequence == NULL)) return 0U;
  *enabled = match_shoot_enabled;
  *sequence = match_shoot_permission_sequence;
  return 1U;
}

void APP_Match_Task(void)
{
  uint32_t now = HAL_GetTick();
  BSP_Uart2_Task(); Match_ParseRx(now); Match_PollCanEvents(now);
  if (match_started != 0U && match_forfeited == 0U && match_alive == 0U && match_revive_due_ms != 0U && Match_TimeReached(now, match_revive_due_ms) != 0U) { match_hp = match_hp_limit; Match_SetAlive(1U, now); }
  Match_SendPending(now); Match_SendStatus(now); BSP_Uart2_Task();
}
