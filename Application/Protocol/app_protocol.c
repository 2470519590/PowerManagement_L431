#include "app_protocol.h"
#include "app_match.h"
#include "app_power.h"
#include "app_armor_enum.h"
#include "app_referee_can.h"
#include "bsp_uart.h"
#include <stdio.h>
#include <string.h>

#define APP_PROTOCOL_RX_MAX_LENGTH 120U
#define APP_PROTOCOL_TX_QUEUE_DEPTH 8U

static char protocol_rx_buffer[APP_PROTOCOL_RX_MAX_LENGTH];
static uint16_t protocol_rx_length;
static uint8_t protocol_rx_overflow;
static uint8_t protocol_tx_queue[APP_PROTOCOL_TX_QUEUE_DEPTH][BSP_UART_TX_MAX_LENGTH];
static uint16_t protocol_tx_length[APP_PROTOCOL_TX_QUEUE_DEPTH];
static uint8_t protocol_tx_head;
static uint8_t protocol_tx_tail;
static uint8_t protocol_status_stream_enabled;
static uint8_t protocol_status_pending;
static PowerReportSnapshot_t protocol_status_snapshot;
static uint32_t protocol_status_sequence;
static uint32_t protocol_last_status_sequence;
static uint32_t protocol_last_fire_event_sequence;
static uint8_t protocol_identity_waiting;

static uint8_t APP_Protocol_ParseUnsigned(const char *text, uint32_t *value);
static uint8_t APP_Protocol_ParseDeciJoule(const char *text, uint16_t *value);
static uint8_t APP_Protocol_ParseSwitchState(const char *text,
                                              PowerSwitchState_t *state);
static uint8_t APP_Protocol_Send(const char *body);
static void APP_Protocol_TxTask(void);
static void APP_Protocol_HandleLine(char *line);
static void APP_Protocol_SendStatus(void);
static void APP_Protocol_SendRaw(void);
static void APP_Protocol_SendFireEvent(void);
static void APP_Protocol_SendArmorList(void);
static void APP_Protocol_PollArmorIdentity(void);

void APP_Protocol_Init(void)
{
  BSP_Uart_Init();
  protocol_rx_length = 0U;
  protocol_rx_overflow = 0U;
  protocol_tx_head = 0U;
  protocol_tx_tail = 0U;
  protocol_status_stream_enabled = 0U;
  protocol_status_pending = 0U;
  protocol_status_snapshot = (PowerReportSnapshot_t){0};
  protocol_status_sequence = 0U;
  protocol_last_status_sequence = 0U;
  protocol_last_fire_event_sequence = 0U;
  protocol_identity_waiting = 0U;
}

void APP_Protocol_Task(void)
{
  uint8_t byte;

  BSP_Uart_Task();
  APP_Protocol_TxTask();
  while (BSP_Uart_ReadByte(&byte) != 0U)
  {
    if (byte == '\n')
    {
      if (protocol_rx_overflow == 0U)
      {
        protocol_rx_buffer[protocol_rx_length] = '\0';
        if (protocol_rx_length != 0U)
        {
          APP_Protocol_HandleLine(protocol_rx_buffer);
        }
      }
      protocol_rx_length = 0U;
      protocol_rx_overflow = 0U;
      continue;
    }

    if ((byte != '\r') && (protocol_rx_overflow == 0U) &&
        (protocol_rx_length < (APP_PROTOCOL_RX_MAX_LENGTH - 1U)))
    {
      protocol_rx_buffer[protocol_rx_length++] = (char)byte;
    }
    else if ((byte != '\r') && (protocol_rx_overflow == 0U))
    {
      protocol_rx_length = 0U;
      protocol_rx_overflow = 1U;
    }
  }

  APP_Protocol_SendFireEvent();
  APP_Protocol_PollArmorIdentity();
  APP_Protocol_SendStatus();
  APP_Protocol_TxTask();
  BSP_Uart_Task();
}

static uint8_t APP_Protocol_ParseUnsigned(const char *text, uint32_t *value)
{
  uint32_t parsed = 0U;

  if ((text == NULL) || (value == NULL) || (*text == '\0'))
  {
    return 0U;
  }
  while (*text != '\0')
  {
    if ((*text < '0') || (*text > '9') || (parsed > 429496729U) ||
        ((parsed == 429496729U) && (*text > '5')))
    {
      return 0U;
    }
    parsed = parsed * 10U + (uint32_t)(*text - '0');
    text++;
  }
  *value = parsed;
  return 1U;
}

static uint8_t APP_Protocol_ParseDeciJoule(const char *text, uint16_t *value)
{
  uint32_t joule;
  const char *decimal_point;
  char integer_part[6];
  uint16_t integer_length;
  uint8_t fraction = 0U;

  if ((text == NULL) || (value == NULL))
  {
    return 0U;
  }
  decimal_point = strchr(text, '.');
  if (decimal_point == NULL)
  {
    if (APP_Protocol_ParseUnsigned(text, &joule) == 0U)
    {
      return 0U;
    }
  }
  else
  {
    integer_length = (uint16_t)(decimal_point - text);
    if ((integer_length == 0U) || (integer_length >= sizeof(integer_part)) ||
        (decimal_point[1] < '0') || (decimal_point[1] > '9') ||
        (decimal_point[2] != '\0'))
    {
      return 0U;
    }
    memcpy(integer_part, text, integer_length);
    integer_part[integer_length] = '\0';
    if (APP_Protocol_ParseUnsigned(integer_part, &joule) == 0U)
    {
      return 0U;
    }
    fraction = (uint8_t)(decimal_point[1] - '0');
  }

  if ((joule > 65U) || ((joule * 10U + fraction) > 65535U))
  {
    return 0U;
  }
  *value = (uint16_t)(joule * 10U + fraction);
  return 1U;
}

static uint8_t APP_Protocol_ParseSwitchState(const char *text,
                                             PowerSwitchState_t *state)
{
  if ((text == NULL) || (state == NULL))
  {
    return 0U;
  }
  if (strcmp(text, "ON") == 0)
  {
    *state = POWER_SWITCH_ON;
    return 1U;
  }
  if (strcmp(text, "OFF") == 0)
  {
    *state = POWER_SWITCH_OFF;
    return 1U;
  }
  return 0U;
}

static uint8_t APP_Protocol_Send(const char *body)
{
  uint16_t body_length;
  uint16_t index;
  uint8_t next_head;

  if (body == NULL)
  {
    return 0U;
  }
  body_length = (uint16_t)strlen(body);
  if ((body_length == 0U) || (body_length > (BSP_UART_TX_MAX_LENGTH - 2U)))
  {
    return 0U;
  }

  next_head = (uint8_t)((protocol_tx_head + 1U) % APP_PROTOCOL_TX_QUEUE_DEPTH);
  if (next_head == protocol_tx_tail)
  {
    return 0U;
  }
  for (index = 0U; index < body_length; index++)
  {
    protocol_tx_queue[protocol_tx_head][index] = (uint8_t)body[index];
  }
  protocol_tx_queue[protocol_tx_head][body_length] = '\r';
  protocol_tx_queue[protocol_tx_head][body_length + 1U] = '\n';
  protocol_tx_length[protocol_tx_head] = body_length + 2U;
  protocol_tx_head = next_head;
  return 1U;
}

static void APP_Protocol_TxTask(void)
{
  while (protocol_tx_tail != protocol_tx_head)
  {
    if (BSP_Uart_Send(protocol_tx_queue[protocol_tx_tail],
                      protocol_tx_length[protocol_tx_tail]) == 0U)
    {
      return;
    }
    protocol_tx_tail =
        (uint8_t)((protocol_tx_tail + 1U) % APP_PROTOCOL_TX_QUEUE_DEPTH);
  }
}

static void APP_Protocol_HandleLine(char *line)
{
  uint32_t value;
  uint16_t buffer_deci_j;
  PowerSwitchState_t switch_state;
  const char *ack_name = "CMD";
  uint8_t command_ok = 0U;

  if (strcmp(line, "ARMOR=LIST") == 0)
  {
    APP_Protocol_SendArmorList();
    return;
  }
  if (strncmp(line, "ARMOR=ID?=", 10U) == 0)
  {
    APP_ArmorIdQueryStatus_t status;
    char message[64];
    if (APP_Protocol_ParseUnsigned(&line[10], &value) == 0U || value > 255U)
    {
      (void)APP_Protocol_Send("ARMOR,ID?,ERR=INVALID_NODE");
      return;
    }
    status = APP_RefereeCan_RequestArmorIdentity((uint8_t)value);
    if (status == APP_ARMOR_ID_QUERY_PENDING)
    {
      (void)snprintf(message, sizeof(message), "ARMOR,ID?,REQ=%lu,PENDING", (unsigned long)value);
      (void)APP_Protocol_Send(message);
      protocol_identity_waiting = 1U;
    }
    else if (status == APP_ARMOR_ID_QUERY_ENUM_NOT_READY)
    {
      (void)APP_Protocol_Send("ARMOR,ID?,ERR=ENUM_NOT_READY");
    }
    else if (status == APP_ARMOR_ID_QUERY_BUSY)
    {
      (void)APP_Protocol_Send("ARMOR,ID?,ERR=BUSY");
    }
    else if (status == APP_ARMOR_ID_QUERY_TX_FAILED)
    {
      (void)APP_Protocol_Send("ARMOR,ID?,ERR=CAN_TX");
    }
    else
    {
      (void)APP_Protocol_Send("ARMOR,ID?,ERR=INVALID_NODE");
    }
    return;
  }

  if (strcmp(line, "PING") == 0)
  {
    (void)APP_Protocol_Send("PONG");
    return;
  }
  else if (strncmp(line, "PWR=", 4U) == 0)
  {
    ack_name = "PWR";
    command_ok = (APP_Protocol_ParseUnsigned(&line[4], &value) != 0U) &&
                 (value <= 65535U) &&
                 (APP_Power_SetApplicationPowerLimit((uint16_t)value) != 0U);
  }
  else if (strncmp(line, "BUF=", 4U) == 0)
  {
    ack_name = "BUF";
    command_ok = (APP_Protocol_ParseDeciJoule(&line[4], &buffer_deci_j) != 0U) &&
                 (APP_Power_SetApplicationBufferEnergy(buffer_deci_j) != 0U);
  }
  else if (strncmp(line, "CUR=", 4U) == 0)
  {
    ack_name = "CUR";
    command_ok = (APP_Protocol_ParseUnsigned(&line[4], &value) != 0U) &&
                 (APP_Power_SetSafetyCurrentLimit(value) != 0U);
  }
  else if (strncmp(line, "SW,CHS,", 7U) == 0)
  {
    ack_name = "CHS";
    command_ok = (APP_Protocol_ParseSwitchState(&line[7], &switch_state) != 0U) &&
                 (APP_Power_ForceChassisState(switch_state) != 0U);
  }
  else if (strncmp(line, "SW,RSV,", 7U) == 0)
  {
    ack_name = "RSV";
    command_ok = (APP_Protocol_ParseSwitchState(&line[7], &switch_state) != 0U) &&
                  (APP_Power_ForceReservedState(switch_state) != 0U);
  }
  else if (strncmp(line, "STA=", 4U) == 0)
  {
    ack_name = "STA";
    if (APP_Protocol_ParseSwitchState(&line[4], &switch_state) != 0U)
    {
      protocol_status_stream_enabled =
          (switch_state == POWER_SWITCH_ON) ? 1U : 0U;
      if (protocol_status_stream_enabled == 0U)
      {
        protocol_status_pending = 0U;
      }
      command_ok = 1U;
    }
  }
  else if (strcmp(line, "GET=STA") == 0)
  {
    ack_name = "STA";
    command_ok = APP_Power_GetLatestReportSnapshot(&protocol_status_snapshot,
                                                     &protocol_status_sequence);
    if (command_ok != 0U)
    {
      protocol_status_pending = 1U;
    }
  }
  else if (strcmp(line, "GET=RAW") == 0)
  {
    ack_name = "RAW";
    command_ok = 1U;
  }
  else if (strcmp(line, "REQ,CHS,ON") == 0)
  {
    ack_name = "CHS";
    command_ok = APP_Power_RequestChassisOn();
  }
  else if (strcmp(line, "REQ,RSV,ON") == 0)
  {
    ack_name = "RSV";
    command_ok = APP_Power_RequestReservedOn();
  }

  {
    char ack[20];
    (void)snprintf(ack, sizeof(ack), "ACK,%s,%s", ack_name,
                   (command_ok != 0U) ? "OK" : "ERR");
    (void)APP_Protocol_Send(ack);
  }

  if ((command_ok != 0U) && (strcmp(line, "GET=RAW") == 0))
  {
    APP_Protocol_SendRaw();
  }
}

static void APP_Protocol_SendArmorList(void)
{
  uint8_t i;
  for (i = 0U; i < APP_ARMOR_ENUM_REQUIRED_NODE_COUNT; i++)
  {
    const APP_ArmorEnumNode_t *e = &armor_enum_diag.nodes[i];
    const APP_ArmorNodeMonitor_t *m = &referee_can_monitor.armor_nodes[i];
    char message[120];
    int length = snprintf(message, sizeof(message),
                          "ARMOR,LIST,N=%u,UID=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X,TOKEN=%06lX,ACK=%u,ONLINE=%u,LAST=%lu,STATE=%u,FAULT=%04X",
                          (unsigned int)e->node_id,
                          (unsigned int)e->uid[0], (unsigned int)e->uid[1],
                          (unsigned int)e->uid[2], (unsigned int)e->uid[3],
                          (unsigned int)e->uid[4], (unsigned int)e->uid[5],
                          (unsigned int)e->uid[6], (unsigned int)e->uid[7],
                          (unsigned int)e->uid[8], (unsigned int)e->uid[9],
                          (unsigned int)e->uid[10], (unsigned int)e->uid[11],
                          (unsigned long)e->token, (unsigned int)e->acked,
                          (unsigned int)m->online, (unsigned long)m->last_rx_tick,
                          (unsigned int)m->state, (unsigned int)m->fault_flags);
    if (length <= 0 || (uint16_t)length >= sizeof(message) || APP_Protocol_Send(message) == 0U)
    {
      (void)APP_Protocol_Send("ARMOR,LIST,ERR=TX_QUEUE");
      return;
    }
  }
}

static void APP_Protocol_PollArmorIdentity(void)
{
  APP_ArmorIdQuery_t query;
  char message[96];
  int length;
  if (protocol_identity_waiting == 0U) { return; }
  APP_RefereeCan_GetArmorIdentityQuery(&query);
  if (query.status == APP_ARMOR_ID_QUERY_PENDING) { return; }
  protocol_identity_waiting = 0U;
  if (query.status == APP_ARMOR_ID_QUERY_OK || query.status == APP_ARMOR_ID_QUERY_MISMATCH)
  {
    length = snprintf(message, sizeof(message),
                      "ARMOR,ID?,REQ=%u,REPLY=%u,TOKEN=%06lX,CRC=%04X,%s",
                      (unsigned int)query.requested_node_id,
                      (unsigned int)query.reply_node_id,
                      (unsigned long)query.reply_token,
                      (unsigned int)query.reply_uid_crc16,
                      (query.status == APP_ARMOR_ID_QUERY_OK) ? "OK" : "MISMATCH");
  }
  else if (query.status == APP_ARMOR_ID_QUERY_TIMEOUT)
  {
    length = snprintf(message, sizeof(message), "ARMOR,ID?,REQ=%u,TIMEOUT",
                      (unsigned int)query.requested_node_id);
  }
  else
  {
    length = snprintf(message, sizeof(message), "ARMOR,ID?,REQ=%u,ERR",
                      (unsigned int)query.requested_node_id);
  }
  if (length > 0 && (uint16_t)length < sizeof(message)) { (void)APP_Protocol_Send(message); }
}

static void APP_Protocol_SendStatus(void)
{
  char status[128];
  int length;

  if (protocol_status_pending == 0U)
  {
    if (protocol_status_stream_enabled == 0U)
    {
      return;
    }
    if (APP_Power_GetLatestReportSnapshot(&protocol_status_snapshot,
                                           &protocol_status_sequence) == 0U)
    {
      return;
    }
    if (protocol_status_sequence == protocol_last_status_sequence)
    {
      return;
    }
    protocol_status_pending = 1U;
  }

  length = snprintf(status, sizeof(status),
                    "STA,PAVG=%lu,IAVG=%lu,OV=%u,SAT=%u,CUT=%u,OCP=%u,BUF=%u.%u,LIM=%u,CUR=%lu,CHS=%s,RSV=%s,PROT=%s",
                    (unsigned long)protocol_status_snapshot.chassis_power_avg_w,
                    (unsigned long)protocol_status_snapshot.chassis_current_avg_ma,
                    (unsigned int)protocol_status_snapshot.power_limit_exceeded,
                    (unsigned int)protocol_status_snapshot.measurement_saturation_risk,
                    (unsigned int)protocol_status_snapshot.power_cut_active,
                    (unsigned int)protocol_status_snapshot.current_overload_active,
                    (unsigned int)(protocol_status_snapshot.power_buffer_deci_j / 10U),
                    (unsigned int)(protocol_status_snapshot.power_buffer_deci_j % 10U),
                    (unsigned int)protocol_status_snapshot.power_limit_w,
                    (unsigned long)protocol_status_snapshot.current_limit_ma,
                    (protocol_status_snapshot.switch_chassis == POWER_SWITCH_ON) ? "ON" : "OFF",
                    (protocol_status_snapshot.switch_reserved == POWER_SWITCH_ON) ? "ON" : "OFF",
                    (protocol_status_snapshot.fire_prohibited == 0U) ? "ON" : "OFF");
  if ((length > 0) && ((uint16_t)length < sizeof(status)) &&
      (APP_Protocol_Send(status) != 0U))
  {
    protocol_status_pending = 0U;
    protocol_last_status_sequence = protocol_status_sequence;
  }
}

static void APP_Protocol_SendFireEvent(void)
{
  uint8_t enabled;
  uint32_t sequence;
  char message[48];
  int length;

  if ((APP_Match_GetShootPermission(&enabled, &sequence) == 0U) ||
      (sequence == protocol_last_fire_event_sequence))
  {
    return;
  }
  length = snprintf(message, sizeof(message), enabled != 0U ? "FIRE=ON" : "FIRE=OFF");
  if ((length > 0) && ((uint16_t)length < sizeof(message)) &&
      (APP_Protocol_Send(message) != 0U))
  {
    protocol_last_fire_event_sequence = sequence;
  }
}

static void APP_Protocol_SendRaw(void)
{
  PowerRawSnapshot_t raw;
  char message[112];
  int length;

  APP_Power_GetRawSnapshot(&raw);
  length = snprintf(message, sizeof(message),
                    "RAW,ADC=%u,I=%lu,P=%lu,AST=%lu,AOK=%lu,AERR=%lu,SAT=%u",
                    (unsigned int)raw.adc_chassis_raw,
                    (unsigned long)raw.chassis_current_ma,
                    (unsigned long)raw.chassis_power_w,
                    (unsigned long)raw.adc_start_count,
                    (unsigned long)raw.adc_success_count,
                    (unsigned long)raw.adc_consecutive_failure_count,
                    (unsigned int)raw.measurement_saturation_risk);
  if ((length > 0) && ((uint16_t)length < sizeof(message)))
  {
    (void)APP_Protocol_Send(message);
  }
}
