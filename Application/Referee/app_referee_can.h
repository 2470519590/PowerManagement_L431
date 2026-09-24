#ifndef APP_REFEREE_CAN_H
#define APP_REFEREE_CAN_H

#include <stdint.h>
#include "app_armor_enum.h"

typedef struct
{
  uint8_t uid[APP_ARMOR_ENUM_UID_BYTES];
  uint8_t node_id;
  uint8_t online;
  uint32_t last_rx_tick;
  uint32_t hit_event_count;
  uint32_t last_hit_sum_peak;
  uint16_t last_hit_force_01n;
  uint16_t fault_flags;
  uint16_t temperature_fault_flags;
  uint8_t last_hit_channel;
  uint8_t last_hit_intensity;
  uint8_t last_hit_sequence;
  uint8_t hit_sequence_valid;
  uint8_t state;
  uint8_t reset_cause;
  uint8_t can_health;
  uint8_t boot_received;
  uint8_t control_ack_received;
  uint8_t control_ack_command;
  uint8_t control_ack_parameter;
} APP_ArmorNodeMonitor_t;

typedef enum
{
  APP_ARMOR_ID_QUERY_IDLE = 0,
  APP_ARMOR_ID_QUERY_PENDING,
  APP_ARMOR_ID_QUERY_OK,
  APP_ARMOR_ID_QUERY_TIMEOUT,
  APP_ARMOR_ID_QUERY_INVALID_NODE,
  APP_ARMOR_ID_QUERY_ENUM_NOT_READY,
  APP_ARMOR_ID_QUERY_BUSY,
  APP_ARMOR_ID_QUERY_TX_FAILED,
  APP_ARMOR_ID_QUERY_MISMATCH
} APP_ArmorIdQueryStatus_t;

#define APP_ARMOR_OFFSET_PARAM_SET 0x0Bu
#define APP_ARMOR_OFFSET_PARAM_ACK 0x0Cu
#define APP_ARMOR_PARAM_P04_THR_HIT 0x04u
#define APP_ARMOR_HIT_THRESHOLD_MIN 1000u
#define APP_ARMOR_HIT_THRESHOLD_MAX 67108864u

typedef enum
{
  APP_ARMOR_THRESHOLD_IDLE = 0,
  APP_ARMOR_THRESHOLD_PENDING,
  APP_ARMOR_THRESHOLD_OK,
  APP_ARMOR_THRESHOLD_TIMEOUT,
  APP_ARMOR_THRESHOLD_INVALID_NODE,
  APP_ARMOR_THRESHOLD_ENUM_NOT_READY,
  APP_ARMOR_THRESHOLD_BUSY,
  APP_ARMOR_THRESHOLD_TX_FAILED,
  APP_ARMOR_THRESHOLD_REJECTED
} APP_ArmorThresholdStatus_t;

typedef struct
{
  APP_ArmorThresholdStatus_t status;
  uint8_t requested_node_id;
  uint8_t sequence;
  uint32_t requested_value;
  uint32_t applied_value;
  uint8_t result;
  uint32_t deadline_ms;
} APP_ArmorThresholdRequest_t;

typedef struct
{
  APP_ArmorIdQueryStatus_t status;
  uint8_t requested_node_id;
  uint8_t reply_node_id;
  uint32_t reply_token;
  uint16_t reply_uid_crc16;
  uint32_t deadline_ms;
} APP_ArmorIdQuery_t;

typedef struct
{
  uint32_t received_frame_count;
  uint32_t invalid_frame_count;
  uint32_t shot_event_count;
  uint32_t query_tx_count;
  uint32_t query_tx_error_count;
  uint32_t gun_query_tx_count;
  uint32_t gun_query_tx_error_count;
  uint32_t armor_query_tx_count;
  uint32_t armor_query_tx_error_count;
  uint32_t last_rx_tick;
  uint32_t shot_count;
  uint16_t last_speed_centimeter_per_second;
  uint8_t barrel_mask;
  uint8_t heat_level;
  uint8_t strong_fault_mask;
  uint8_t weak_fault_mask;
  uint8_t gun_online;
  uint8_t boot_received;
  uint8_t calibration_ack_received;
  uint8_t control_ack_received;
  uint16_t calibration_front_before;
  uint16_t calibration_front_after;
  uint16_t calibration_rear_before;
  uint16_t calibration_rear_after;
  uint8_t control_ack_command;
  uint8_t control_ack_parameter;
  uint32_t armor_last_rx_tick;
  uint32_t armor_hit_event_count;
  uint32_t armor_last_hit_sum_peak;
  uint16_t armor_last_hit_force_01n;
  uint16_t armor_fault_flags;
  uint16_t armor_temperature_fault_flags;
  uint8_t armor_last_hit_channel;
  uint8_t armor_last_hit_intensity;
  uint8_t armor_state;
  uint8_t armor_reset_cause;
  uint8_t armor_can_health;
  uint8_t armor_online;
  uint8_t armor_boot_received;
  uint8_t armor_control_ack_received;
  uint8_t armor_control_ack_command;
  uint8_t armor_control_ack_parameter;
  uint8_t armor_enum_ready;
  uint32_t armor_reenum_request_count;
  uint32_t armor_reenum_last_ms;
  uint8_t armor_reenum_last_offline_node;
  APP_ArmorNodeMonitor_t armor_nodes[APP_ARMOR_ENUM_NODE_COUNT];
} APP_RefereeCanMonitor_t;

extern APP_RefereeCanMonitor_t referee_can_monitor;

/* Host-controlled maintenance gate used while armor boards are upgraded. */
#define APP_REFEREE_CAN_MAINT_CMD_ID 0x110U
#define APP_REFEREE_CAN_MAINT_ACK_ID 0x111U
#define APP_REFEREE_CAN_MAINT_MAGIC 0xA5U
#define APP_REFEREE_CAN_MAINT_ACK_MAGIC 0x5AU
#define APP_REFEREE_CAN_MAINT_ENTER 1U
#define APP_REFEREE_CAN_MAINT_EXIT 0U

void APP_RefereeCan_Init(void);
void APP_RefereeCan_Task(void);
uint8_t APP_RefereeCan_IsMaintenanceSilent(void);
/* Idempotent command. The CAN task retries until the gun returns 0x234. */
void APP_RefereeCan_RequestGunHeat(uint8_t heat);
APP_ArmorIdQueryStatus_t APP_RefereeCan_RequestArmorIdentity(uint8_t node_id);
void APP_RefereeCan_GetArmorIdentityQuery(APP_ArmorIdQuery_t *query);
APP_ArmorThresholdStatus_t APP_RefereeCan_RequestArmorHitThreshold(uint8_t node_id, uint32_t threshold);
void APP_RefereeCan_GetArmorThresholdRequest(APP_ArmorThresholdRequest_t *request);
void APP_RefereeCan_ClearArmorThresholdRequest(void);

#endif /* APP_REFEREE_CAN_H */
