#include "app_armor_enum.h"
#include <string.h>

#define APP_ARMOR_ENUM_FRAGMENT_COUNT 4u
#define APP_ARMOR_ENUM_FRAGMENT_MASK 0x0Fu
/* 收集窗口必须覆盖装甲板侧 4 s 的枚举生命周期，否则 L431 会在装甲板
 * 自己完成重试前再次发送 0x120，反复清除在线节点的枚举状态。 */
#define APP_ARMOR_ENUM_COLLECT_TIMEOUT_MS 4500u
#define APP_ARMOR_ENUM_RESTART_DELAY_MS 300u     /* 100→300 */
#define APP_ARMOR_ENUM_ASSIGN_TIMEOUT_MS 100u
#define APP_ARMOR_ENUM_ASSIGN_RETRY_LIMIT 3u
#define APP_ARMOR_ENUM_ASSIGN_RETRY_DELAY_MS 20u
#define APP_ARMOR_ENUM_ACK_MARKER 0xA5u

APP_ArmorEnumDiag_t armor_enum_diag;
static uint32_t s_deadline_ms;
static uint32_t s_restart_due_ms;
static uint32_t s_start_retry_due_ms;
static uint8_t s_have_assigned_nodes;

static uint32_t ArmorEnum_Fnv1a(const uint8_t *data, uint8_t len)
{
  uint32_t hash = 2166136261u; uint8_t i;
  for (i = 0u; i < len; i++) { hash ^= data[i]; hash *= 16777619u; }
  return hash;
}

static uint16_t ArmorEnum_Crc16Ccitt(const uint8_t *data, uint8_t len)
{
  uint16_t crc = 0xFFFFu; uint8_t i;
  while (len-- != 0u) { crc ^= (uint16_t)(*data++) << 8; for (i = 0u; i < 8u; i++) crc = ((crc & 0x8000u) != 0u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1); }
  return crc;
}

static int8_t ArmorEnum_FindToken(uint32_t token)
{
  uint8_t i;
  for (i = 0u; i < armor_enum_diag.collected_count; i++) if (armor_enum_diag.nodes[i].token == token) return (int8_t)i;
  return -1;
}

static void ArmorEnum_SortNodes(void)
{
  uint8_t i; uint8_t j;
  for (i = 0u; i < APP_ARMOR_ENUM_REQUIRED_NODE_COUNT; i++)
  {
    for (j = (uint8_t)(i + 1u); j < APP_ARMOR_ENUM_REQUIRED_NODE_COUNT; j++)
    {
      if (memcmp(armor_enum_diag.nodes[i].uid, armor_enum_diag.nodes[j].uid, APP_ARMOR_ENUM_UID_BYTES) > 0)
      {
        APP_ArmorEnumNode_t temp = armor_enum_diag.nodes[i]; armor_enum_diag.nodes[i] = armor_enum_diag.nodes[j]; armor_enum_diag.nodes[j] = temp;
      }
    }
    armor_enum_diag.nodes[i].node_id = (uint8_t)(i + 1u);
  }
}

/* The start broadcast itself may lose arbitration or hit a transient CAN
 * error.  Do not enter a 2.5 s collect window until it has really left the
 * controller; retry the same session shortly instead. */
static void ArmorEnum_SendStart(uint32_t now)
{
  uint8_t start_data = armor_enum_diag.session;
  if (BSP_Can_Send(APP_ARMOR_ENUM_ID_START, &start_data, 1u) != 0u)
  {
    armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT;
    s_deadline_ms = now + APP_ARMOR_ENUM_COLLECT_TIMEOUT_MS;
    return;
  }
  armor_enum_diag.start_tx_fail_count++;
  armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT;
  s_deadline_ms = 0u;
  s_start_retry_due_ms = now + APP_ARMOR_ENUM_ASSIGN_RETRY_DELAY_MS;
}

static void ArmorEnum_StartRound(uint32_t now)
{
  armor_enum_diag.session++;
  if (armor_enum_diag.session == 0u) armor_enum_diag.session = 1u;
  memset(armor_enum_diag.nodes, 0, sizeof(armor_enum_diag.nodes));
  armor_enum_diag.collected_count = 0u; armor_enum_diag.assign_index = 0u; armor_enum_diag.assign_attempt = 0u;
  armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT; armor_enum_diag.round_count++;
  s_start_retry_due_ms = 0u;
  ArmorEnum_SendStart(now);
}

static void ArmorEnum_RestartLater(uint32_t now)
{
  armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT;
  s_restart_due_ms = now + APP_ARMOR_ENUM_RESTART_DELAY_MS;
  s_start_retry_due_ms = 0u;
  s_deadline_ms = 0u;
}

static void ArmorEnum_ScheduleAssignRetry(uint32_t now)
{
  armor_enum_diag.assign_attempt++;
  if (armor_enum_diag.assign_attempt >= APP_ARMOR_ENUM_ASSIGN_RETRY_LIMIT)
  {
    armor_enum_diag.timeout_count++;
    ArmorEnum_RestartLater(now);
    return;
  }

  armor_enum_diag.state = APP_ARMOR_ENUM_ASSIGN_RETRY;
  s_deadline_ms = now + APP_ARMOR_ENUM_ASSIGN_RETRY_DELAY_MS;
}

static void ArmorEnum_SendAssign(uint32_t now)
{
  APP_ArmorEnumNode_t *node = &armor_enum_diag.nodes[armor_enum_diag.assign_index];
  uint8_t data[8] = {0u};
  data[0] = armor_enum_diag.session; data[1] = node->node_id;
  data[2] = (uint8_t)node->token; data[3] = (uint8_t)(node->token >> 8); data[4] = (uint8_t)(node->token >> 16);
  data[5] = (uint8_t)node->uid_crc16; data[6] = (uint8_t)(node->uid_crc16 >> 8);
  if (BSP_Can_Send(APP_ARMOR_ENUM_ID_ASSIGN, data, sizeof(data)) != 0u)
  {
    armor_enum_diag.state = APP_ARMOR_ENUM_WAIT_ACK; s_deadline_ms = now + APP_ARMOR_ENUM_ASSIGN_TIMEOUT_MS;
  }
  else
  {
    ArmorEnum_ScheduleAssignRetry(now);
  }
}

void APP_ArmorEnum_Init(uint32_t now)
{
  memset(&armor_enum_diag, 0, sizeof(armor_enum_diag));
  s_have_assigned_nodes = 0u;
  ArmorEnum_StartRound(now);
}

void APP_ArmorEnum_ForceRestart(uint32_t now)
{
  /* A board can reset independently while the referee MCU remains alive.
   * Begin a fresh session immediately so an unassigned board can rejoin. */
  s_restart_due_ms = 0u;
  s_start_retry_due_ms = 0u;
  ArmorEnum_StartRound(now);
}

uint8_t APP_ArmorEnum_OnFrame(const BSP_CanFrame_t *frame, uint32_t now)
{
  int8_t index;
  uint32_t token;
  uint8_t fragment;
  APP_ArmorEnumNode_t *node;
  if (frame == NULL || (frame->standard_id != APP_ARMOR_ENUM_ID_ANNOUNCE && frame->standard_id != APP_ARMOR_ENUM_ID_ACK)) return 0u;

  if (frame->standard_id == APP_ARMOR_ENUM_ID_ACK)
  {
    if (frame->dlc != 8u || armor_enum_diag.state != APP_ARMOR_ENUM_WAIT_ACK || frame->data[0] != armor_enum_diag.session || frame->data[7] != APP_ARMOR_ENUM_ACK_MARKER) { armor_enum_diag.invalid_frame_count++; return 1u; }
    node = &armor_enum_diag.nodes[armor_enum_diag.assign_index];
    token = (uint32_t)frame->data[2] | ((uint32_t)frame->data[3] << 8) | ((uint32_t)frame->data[4] << 16);
    if (frame->data[1] != node->node_id || token != node->token || ((uint16_t)frame->data[5] | ((uint16_t)frame->data[6] << 8)) != node->uid_crc16) { armor_enum_diag.invalid_frame_count++; return 1u; }
    node->acked = 1u; armor_enum_diag.assign_index++; armor_enum_diag.assign_attempt = 0u;
    if (armor_enum_diag.assign_index >= APP_ARMOR_ENUM_REQUIRED_NODE_COUNT)
    {
      armor_enum_diag.state = APP_ARMOR_ENUM_READY;
      s_have_assigned_nodes = 1u;
    }
    else
    {
      armor_enum_diag.state = APP_ARMOR_ENUM_ASSIGN;
    }
    return 1u;
  }

  if (frame->dlc != 8u || armor_enum_diag.state != APP_ARMOR_ENUM_COLLECT || s_restart_due_ms != 0u || frame->data[0] != armor_enum_diag.session || frame->data[1] >= APP_ARMOR_ENUM_FRAGMENT_COUNT) { armor_enum_diag.invalid_frame_count++; return 1u; }
  fragment = frame->data[1]; token = (uint32_t)frame->data[5] | ((uint32_t)frame->data[6] << 8) | ((uint32_t)frame->data[7] << 16);
  index = ArmorEnum_FindToken(token);
  /* 同一 token 的重发必须复用原候选。否则三块板持续重发时，重复候选
   * 会耗尽 collected_count，造成无意义的 overflow/restart。若确有 24-bit
   * token 碰撞，后面的完整 token 校验会拒绝本轮并触发重试。 */
  if (index < 0)
  {
    if (armor_enum_diag.collected_count >= APP_ARMOR_ENUM_NODE_COUNT) { armor_enum_diag.overflow_count++; ArmorEnum_RestartLater(now); return 1u; }
    index = (int8_t)armor_enum_diag.collected_count++; armor_enum_diag.nodes[(uint8_t)index].token = token;
  }
  node = &armor_enum_diag.nodes[(uint8_t)index];
  memcpy(&node->uid[fragment * 3u], &frame->data[2], 3u);
  node->fragment_mask |= (uint8_t)(1u << fragment);
  if (node->fragment_mask == APP_ARMOR_ENUM_FRAGMENT_MASK)
  {
    uint32_t full_token = ArmorEnum_Fnv1a(node->uid, APP_ARMOR_ENUM_UID_BYTES) & 0x00FFFFFFu;
    node->uid_crc16 = ArmorEnum_Crc16Ccitt(node->uid, APP_ARMOR_ENUM_UID_BYTES);
    if (full_token != node->token) { armor_enum_diag.invalid_frame_count++; ArmorEnum_RestartLater(now); return 1u; }
    {
      uint8_t i;
      for (i = 0u; i < armor_enum_diag.collected_count; i++)
      {
        if ((uint8_t)i != (uint8_t)index &&
            armor_enum_diag.nodes[i].fragment_mask == APP_ARMOR_ENUM_FRAGMENT_MASK &&
            memcmp(armor_enum_diag.nodes[i].uid, node->uid, APP_ARMOR_ENUM_UID_BYTES) == 0)
        {
          /* 同一节点的 announce 重发：保留先前完整项，丢弃新候选项。 */
          if ((uint8_t)index == (uint8_t)(armor_enum_diag.collected_count - 1u))
          {
            memset(node, 0, sizeof(*node));
            armor_enum_diag.collected_count--;
          }
          return 1u;
        }
      }
    }
    if (armor_enum_diag.collected_count == APP_ARMOR_ENUM_REQUIRED_NODE_COUNT)
    {
      uint8_t i;
      for (i = 0u; i < APP_ARMOR_ENUM_REQUIRED_NODE_COUNT; i++) if (armor_enum_diag.nodes[i].fragment_mask != APP_ARMOR_ENUM_FRAGMENT_MASK) return 1u;
      ArmorEnum_SortNodes(); armor_enum_diag.assign_index = 0u; armor_enum_diag.assign_attempt = 0u; armor_enum_diag.state = APP_ARMOR_ENUM_ASSIGN;
    }
  }
  return 1u;
}

void APP_ArmorEnum_Task(uint32_t now)
{
  if (s_start_retry_due_ms != 0u && (int32_t)(now - s_start_retry_due_ms) >= 0)
  {
    s_start_retry_due_ms = 0u;
    ArmorEnum_SendStart(now);
    return;
  }
  if (s_restart_due_ms != 0u && (int32_t)(now - s_restart_due_ms) >= 0) { s_restart_due_ms = 0u; ArmorEnum_StartRound(now); return; }
  if (armor_enum_diag.state == APP_ARMOR_ENUM_COLLECT && s_deadline_ms != 0u && (int32_t)(now - s_deadline_ms) >= 0) { armor_enum_diag.timeout_count++; ArmorEnum_RestartLater(now); return; }
  if (armor_enum_diag.state == APP_ARMOR_ENUM_ASSIGN_RETRY && (int32_t)(now - s_deadline_ms) >= 0)
  {
    armor_enum_diag.state = APP_ARMOR_ENUM_ASSIGN;
    return;
  }
  if (armor_enum_diag.state == APP_ARMOR_ENUM_ASSIGN) { ArmorEnum_SendAssign(now); return; }
  if (armor_enum_diag.state == APP_ARMOR_ENUM_WAIT_ACK && (int32_t)(now - s_deadline_ms) >= 0)
  {
    ArmorEnum_ScheduleAssignRetry(now);
  }
}

uint8_t APP_ArmorEnum_IsReady(void) { return armor_enum_diag.state == APP_ARMOR_ENUM_READY; }
uint8_t APP_ArmorEnum_ActiveNodeCount(void)
{
  /* Node-ID business slots are deterministic.  Once one successful round has
   * assigned them, keep serving those slots during a later re-enumeration so
   * healthy boards never hit their 300 ms communication-lost LED timeout. */
  return (s_have_assigned_nodes != 0u) ?
         APP_ARMOR_ENUM_REQUIRED_NODE_COUNT : 0u;
}
uint16_t APP_ArmorEnum_BusinessId(uint8_t node_id, uint8_t offset)
{
  if (node_id == 0u || node_id > APP_ARMOR_ENUM_REQUIRED_NODE_COUNT || offset > APP_ARMOR_BUSINESS_OFFSET_MAX) return 0u;
  return (uint16_t)(APP_ARMOR_BUSINESS_BASE_ID + ((uint16_t)(node_id - 1u) * APP_ARMOR_BUSINESS_STRIDE) + offset);
}
int8_t APP_ArmorEnum_NodeFromBusinessId(uint16_t id, uint8_t offset)
{
  uint16_t relative; uint8_t node;
  if (id < APP_ARMOR_BUSINESS_BASE_ID) return -1;
  relative = (uint16_t)(id - APP_ARMOR_BUSINESS_BASE_ID); node = (uint8_t)(relative / APP_ARMOR_BUSINESS_STRIDE) + 1u;
  if (node > APP_ARMOR_ENUM_REQUIRED_NODE_COUNT || (relative % APP_ARMOR_BUSINESS_STRIDE) != offset) return -1;
  return (int8_t)node;
}
