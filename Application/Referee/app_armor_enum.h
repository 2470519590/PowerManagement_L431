#ifndef APP_ARMOR_ENUM_H
#define APP_ARMOR_ENUM_H

#include <stdint.h>
#include "bsp_can.h"

#define APP_ARMOR_ENUM_NODE_COUNT 4u
/* 正式版本保持 4；测试固件可覆盖为 1~3，允许部分装甲板完成枚举。 */
#ifndef APP_ARMOR_ENUM_REQUIRED_NODE_COUNT
#define APP_ARMOR_ENUM_REQUIRED_NODE_COUNT APP_ARMOR_ENUM_NODE_COUNT
#endif
#if (APP_ARMOR_ENUM_REQUIRED_NODE_COUNT < 1u) || (APP_ARMOR_ENUM_REQUIRED_NODE_COUNT > APP_ARMOR_ENUM_NODE_COUNT)
#error "APP_ARMOR_ENUM_REQUIRED_NODE_COUNT must be in range 1..4"
#endif
#define APP_ARMOR_ENUM_UID_BYTES 12u
#define APP_ARMOR_ENUM_ID_START 0x120u
#define APP_ARMOR_ENUM_ID_ANNOUNCE 0x121u
#define APP_ARMOR_ENUM_ID_ASSIGN 0x122u
#define APP_ARMOR_ENUM_ID_ACK 0x123u
#define APP_ARMOR_BUSINESS_BASE_ID 0x130u
#define APP_ARMOR_BUSINESS_STRIDE 0x10u
/* Per-node IDs use a stride of 0x10. Offset 0xA is the reliable HIT ACK;
 * keep this limit in sync with the armor-board firmware. */
#define APP_ARMOR_BUSINESS_OFFSET_MAX 0xAu

typedef enum { APP_ARMOR_ENUM_COLLECT = 0, APP_ARMOR_ENUM_ASSIGN, APP_ARMOR_ENUM_ASSIGN_RETRY, APP_ARMOR_ENUM_WAIT_ACK, APP_ARMOR_ENUM_READY } APP_ArmorEnumState_t;

typedef struct
{
  uint8_t uid[APP_ARMOR_ENUM_UID_BYTES];
  uint32_t token;
  uint16_t uid_crc16;
  uint8_t fragment_mask;
  uint8_t node_id;
  uint8_t acked;
} APP_ArmorEnumNode_t;

typedef struct
{
  APP_ArmorEnumNode_t nodes[APP_ARMOR_ENUM_NODE_COUNT];
  APP_ArmorEnumState_t state;
  uint8_t session;
  uint8_t collected_count;
  uint8_t assign_index;
  uint8_t assign_attempt;
  uint32_t round_count;
  uint32_t invalid_frame_count;
  uint32_t overflow_count;
  uint32_t timeout_count;
  uint32_t start_tx_fail_count;
} APP_ArmorEnumDiag_t;

extern APP_ArmorEnumDiag_t armor_enum_diag;

void APP_ArmorEnum_Init(uint32_t now);
void APP_ArmorEnum_Task(uint32_t now);
void APP_ArmorEnum_ForceRestart(uint32_t now);
uint8_t APP_ArmorEnum_OnFrame(const BSP_CanFrame_t *frame, uint32_t now);
uint8_t APP_ArmorEnum_IsReady(void);
uint8_t APP_ArmorEnum_ActiveNodeCount(void);
uint16_t APP_ArmorEnum_BusinessId(uint8_t node_id, uint8_t offset);
int8_t APP_ArmorEnum_NodeFromBusinessId(uint16_t id, uint8_t offset);

#endif
