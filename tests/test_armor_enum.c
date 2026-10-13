#include "app_armor_enum.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static BSP_CanFrame_t sent;
static uint8_t send_ok = 1;
uint8_t BSP_Can_Send(uint16_t id, const uint8_t *data, uint8_t dlc)
{
  sent = (BSP_CanFrame_t){.standard_id = id, .dlc = dlc};
  if (dlc) memcpy(sent.data, data, dlc);
  return send_ok;
}
static void announce(unsigned board, unsigned fragments, uint32_t now)
{
  uint8_t uid[12]; uint32_t token = 2166136261u;
  for (unsigned i = 0; i < 12; i++) {
    uid[i] = (uint8_t)(board * 17 + i); token = (token ^ uid[i]) * 16777619u;
  }
  for (unsigned i = 0; i < fragments; i++) {
    BSP_CanFrame_t f = {.standard_id = 0x121, .dlc = 8};
    f.data[0] = armor_enum_diag.session; f.data[1] = (uint8_t)i;
    memcpy(f.data + 2, uid + i * 3, 3);
    f.data[5] = (uint8_t)token; f.data[6] = (uint8_t)(token >> 8); f.data[7] = (uint8_t)(token >> 16);
    APP_ArmorEnum_OnFrame(&f, now);
  }
}
static unsigned assign(unsigned board, uint32_t now)
{
  announce(board, 4, now); APP_ArmorEnum_Task(now);
  assert(sent.standard_id == 0x122);
  BSP_CanFrame_t ack = sent; ack.standard_id = 0x123; ack.data[7] = 0xA5;
  APP_ArmorEnum_OnFrame(&ack, now + 1);
  assert(APP_ArmorEnum_IsNodeAssigned(ack.data[1]));
  return ack.data[1];
}
int main(void)
{
  for (unsigned n = 0; n <= 4; n++) {
    APP_ArmorEnum_Init(0); APP_ArmorEnum_Task(1000);
    for (unsigned i = 1; i <= n; i++) {
      assign(i, 1100 + i * 10);
      assert(APP_ArmorEnum_ActiveNodeCount() == i);
    }
    APP_ArmorEnum_Task(3500); assert(APP_ArmorEnum_IsReady());
    assert(APP_ArmorEnum_ActiveNodeCount() == n);
  }
  /* Three boards remain committed during periodic discovery; hot insert joins. */
  APP_ArmorEnum_Init(0); APP_ArmorEnum_Task(1000);
  unsigned a = assign(3, 1100), b = assign(1, 1120), c = assign(2, 1140);
  APP_ArmorEnum_Task(3500); APP_ArmorEnum_Task(8499);
  assert(APP_ArmorEnum_IsReady());
  APP_ArmorEnum_Task(8500); assert(sent.standard_id == 0x120);
  assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  assert(assign(3, 8600) == a); assert(assign(1, 8620) == b); assert(assign(2, 8640) == c);
  unsigned d = assign(4, 8660); assert(APP_ArmorEnum_ActiveNodeCount() == 4);
  APP_ArmorEnum_Task(11000); APP_ArmorEnum_Task(16000);
  assert(APP_ArmorEnum_IsReady());
  /* A middle slot can disappear without moving the other boards. */
  APP_ArmorEnum_MarkOffline(b); assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  APP_ArmorEnum_Task(16001); assert(sent.standard_id == 0x120);
  assert(assign(3, 16100) == a); assert(assign(2, 16120) == c); assert(assign(4, 16140) == d);
  APP_ArmorEnum_Task(18501); assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  APP_ArmorEnum_Task(23501); assert(assign(1, 23600) == b);
  assert(APP_ArmorEnum_ActiveNodeCount() == 4);
  /* Missing ACK on a candidate must not clear the acknowledged peers. */
  APP_ArmorEnum_MarkOffline(d); APP_ArmorEnum_ForceRestart(24000); APP_ArmorEnum_Task(24000);
  announce(4, 4, 24100); APP_ArmorEnum_Task(24100);
  APP_ArmorEnum_Task(24200); APP_ArmorEnum_Task(24300); APP_ArmorEnum_Task(24400);
  assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  assert(assign(2, 24500) == c);
  announce(5, 2, 24600); APP_ArmorEnum_Task(26500);
  assert(APP_ArmorEnum_IsReady()); assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  send_ok = 0; APP_ArmorEnum_Init(0); APP_ArmorEnum_Task(1000); APP_ArmorEnum_Task(1020);
  assert(armor_enum_diag.start_tx_fail_count == 2);
  puts("incremental enumeration and hot-plug scheduler tests passed");
}
