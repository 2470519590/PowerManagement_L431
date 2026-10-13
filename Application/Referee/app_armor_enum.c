#include "app_armor_enum.h"
#include <string.h>

#define COLLECT_MS 2500u
#define SCAN_MS 5000u
APP_ArmorEnumDiag_t armor_enum_diag;
static APP_ArmorEnumNode_t candidates[4];
static uint8_t done[4], pending, slot, start_pending;
static uint32_t collect_due, scan_due, assign_due;

static uint32_t Token(const uint8_t *uid)
{
  uint32_t hash = 2166136261u; uint8_t i;
  for (i = 0; i < 12; i++) hash = (hash ^ uid[i]) * 16777619u;
  return hash & 0xFFFFFFu;
}
static uint16_t Crc(const uint8_t *uid)
{
  uint16_t crc = 0xFFFFu; uint8_t i, b;
  for (i = 0; i < 12; i++) {
    crc ^= (uint16_t)uid[i] << 8;
    for (b = 0; b < 8; b++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}
static void Count(void)
{
  uint8_t i; armor_enum_diag.active_count = 0;
  for (i = 0; i < 4; i++) if (armor_enum_diag.nodes[i].acked) armor_enum_diag.active_count++;
}
void APP_ArmorEnum_MarkOffline(uint8_t id)
{
  if (!id || id > 4) return;
  armor_enum_diag.nodes[id - 1].acked = 0; Count();
}
uint8_t APP_ArmorEnum_IsNodeAssigned(uint8_t id)
{
  return id && id <= 4 && armor_enum_diag.nodes[id - 1].acked;
}
static void Start(uint32_t now)
{
  if (++armor_enum_diag.session == 0) armor_enum_diag.session = 1;
  memset(candidates, 0, sizeof(candidates)); memset(done, 0, sizeof(done));
  armor_enum_diag.collected_count = 0; armor_enum_diag.assign_attempt = 0;
  armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT; armor_enum_diag.round_count++;
  pending = 0; start_pending = 1; assign_due = now;
}
void APP_ArmorEnum_Init(uint32_t now)
{
  memset(&armor_enum_diag, 0, sizeof(armor_enum_diag));
  memset(candidates, 0, sizeof(candidates)); memset(done, 0, sizeof(done));
  pending = start_pending = 0; collect_due = 0; scan_due = now + 1000u;
  armor_enum_diag.state = APP_ARMOR_ENUM_READY;
}
void APP_ArmorEnum_ForceRestart(uint32_t now) { Start(now); }
static int8_t FindSlot(const APP_ArmorEnumNode_t *node)
{
  uint8_t i;
  /* Keep known UIDs in place, including unplugged boards returning later. */
  for (i = 0; i < 4; i++) if (armor_enum_diag.nodes[i].node_id && !memcmp(armor_enum_diag.nodes[i].uid, node->uid, 12)) return (int8_t)i;
  for (i = 0; i < 4; i++) if (!armor_enum_diag.nodes[i].node_id) return (int8_t)i;
  for (i = 0; i < 4; i++) if (!armor_enum_diag.nodes[i].acked) return (int8_t)i;
  return -1;
}
uint8_t APP_ArmorEnum_OnFrame(const BSP_CanFrame_t *f, uint32_t now)
{
  uint8_t i; uint32_t token; APP_ArmorEnumNode_t *node;
  if (!f || (f->standard_id != 0x121u && f->standard_id != 0x123u)) return 0;
  if (f->dlc != 8 || f->data[0] != armor_enum_diag.session || start_pending || armor_enum_diag.state == APP_ARMOR_ENUM_READY) goto invalid;
  token = (uint32_t)f->data[2] | ((uint32_t)f->data[3] << 8) | ((uint32_t)f->data[4] << 16);
  if (f->standard_id == 0x123u) {
    if (!pending || f->data[7] != 0xA5u) goto invalid;
    node = &candidates[armor_enum_diag.assign_index];
    if (f->data[1] != slot + 1 || token != node->token || ((uint16_t)f->data[5] | ((uint16_t)f->data[6] << 8)) != node->uid_crc16) goto invalid;
    armor_enum_diag.nodes[slot] = *node;
    armor_enum_diag.nodes[slot].node_id = slot + 1; armor_enum_diag.nodes[slot].acked = 1;
    done[armor_enum_diag.assign_index] = 1; pending = 0;
    armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT; Count(); return 1;
  }
  if (f->data[1] >= 4 || (int32_t)(now - collect_due) >= 0) goto invalid;
  token = (uint32_t)f->data[5] | ((uint32_t)f->data[6] << 8) | ((uint32_t)f->data[7] << 16);
  for (i = 0; i < armor_enum_diag.collected_count; i++) if (candidates[i].token == token) break;
  if (i == armor_enum_diag.collected_count) {
    if (i >= 4) { armor_enum_diag.overflow_count++; return 1; }
    armor_enum_diag.collected_count++; candidates[i].token = token;
  }
  node = &candidates[i]; if (done[i]) return 1;
  memcpy(node->uid + f->data[1] * 3, f->data + 2, 3);
  node->fragment_mask |= (uint8_t)(1u << f->data[1]);
  if (node->fragment_mask == 0x0Fu) {
    if (Token(node->uid) != token) { node->fragment_mask = 0; goto invalid; }
    node->uid_crc16 = Crc(node->uid);
  }
  return 1;
invalid:
  armor_enum_diag.invalid_frame_count++; return 1;
}
void APP_ArmorEnum_Task(uint32_t now)
{
  uint8_t i;
  if (armor_enum_diag.state == APP_ARMOR_ENUM_READY) {
    if (armor_enum_diag.active_count == 4 || (int32_t)(now - scan_due) < 0) return;
    Start(now);
  }
  if (start_pending) {
    if ((int32_t)(now - assign_due) < 0) return;
    if (!BSP_Can_Send(0x120u, &armor_enum_diag.session, 1)) {
      armor_enum_diag.start_tx_fail_count++; assign_due = now + 20u; return;
    }
    start_pending = 0; collect_due = now + COLLECT_MS; return;
  }
  if (pending) {
    if ((int32_t)(now - assign_due) < 0) return;
    if (armor_enum_diag.assign_attempt >= 3) {
      armor_enum_diag.timeout_count++; done[armor_enum_diag.assign_index] = 1;
      pending = 0; armor_enum_diag.state = APP_ARMOR_ENUM_COLLECT;
    }
  }
  if (!pending) {
    for (i = 0; i < armor_enum_diag.collected_count; i++) if (!done[i] && candidates[i].fragment_mask == 0x0Fu) break;
    if (i == armor_enum_diag.collected_count) {
      if ((int32_t)(now - collect_due) >= 0) { armor_enum_diag.state = APP_ARMOR_ENUM_READY; scan_due = now + SCAN_MS; }
      return;
    }
    {
      int8_t found = FindSlot(&candidates[i]);
      if (found < 0) { done[i] = 1; armor_enum_diag.overflow_count++; return; }
      slot = (uint8_t)found;
    }
    armor_enum_diag.assign_index = i; armor_enum_diag.assign_attempt = 0; pending = 1;
  }
  {
    APP_ArmorEnumNode_t *node = &candidates[armor_enum_diag.assign_index];
    uint8_t data[8] = {armor_enum_diag.session, slot + 1, (uint8_t)node->token,
      (uint8_t)(node->token >> 8), (uint8_t)(node->token >> 16),
      (uint8_t)node->uid_crc16, (uint8_t)(node->uid_crc16 >> 8), 0};
    uint8_t ok = BSP_Can_Send(0x122u, data, 8);
    armor_enum_diag.assign_attempt++; armor_enum_diag.state = APP_ARMOR_ENUM_WAIT_ACK;
    assign_due = now + (ok ? 100u : 20u);
  }
}
uint8_t APP_ArmorEnum_IsReady(void) { return armor_enum_diag.state == APP_ARMOR_ENUM_READY; }
uint8_t APP_ArmorEnum_ActiveNodeCount(void) { return armor_enum_diag.active_count; }
uint16_t APP_ArmorEnum_BusinessId(uint8_t id, uint8_t offset)
{
  if (!id || id > 4 || offset > APP_ARMOR_BUSINESS_OFFSET_MAX) return 0;
  return (uint16_t)(APP_ARMOR_BUSINESS_BASE_ID + (id - 1u) * APP_ARMOR_BUSINESS_STRIDE + offset);
}
int8_t APP_ArmorEnum_NodeFromBusinessId(uint16_t id, uint8_t offset)
{
  uint16_t relative;
  if (id < APP_ARMOR_BUSINESS_BASE_ID || id >= APP_ARMOR_BUSINESS_BASE_ID + 4u * APP_ARMOR_BUSINESS_STRIDE) return -1;
  relative = id - APP_ARMOR_BUSINESS_BASE_ID;
  if (relative % APP_ARMOR_BUSINESS_STRIDE != offset) return -1;
  return (int8_t)(relative / APP_ARMOR_BUSINESS_STRIDE + 1u);
}
