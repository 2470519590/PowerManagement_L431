/* Exercise the production CAN scheduler against boards obeying the existing
 * reset-on-0x120 protocol. No armor firmware modifications are required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../Application/Referee/app_referee_can.c"
static uint32_t Token(const uint8_t *uid);
static uint16_t Crc(const uint8_t *uid);

typedef struct { uint8_t present, id, session, fragment, uid[12]; uint32_t due, last_query; } Board;
static Board boards[4];
static uint32_t tick;
static BSP_CanFrame_t queue[128];
static unsigned rd, wr;
static uint32_t max_gap[4];
static void push(BSP_CanFrame_t f) { assert(wr - rd < 128); queue[wr++ % 128] = f; }
uint8_t BSP_Can_Init(void) { return 1; }
void BSP_Can_Task(void) {}
uint8_t BSP_Time_Register1msCallback(BSP_Time1msCallback_t cb) { (void)cb; return 1; }
uint8_t BSP_Can_Read(BSP_CanFrame_t *f) { if (rd == wr) return 0; *f = queue[rd++ % 128]; return 1; }
uint8_t BSP_Can_Send(uint16_t id, const uint8_t *data, uint8_t dlc)
{
  for (unsigned i = 0; i < 4; i++) {
    Board *b = &boards[i]; if (!b->present) continue;
    if (id == 0x120) {
      b->id = 0; b->session = data[0]; b->fragment = 0; b->due = tick + 5 + i * 20;
    } else if (id == 0x122 && b->session == data[0] && Token(b->uid) == APP_RefereeCan_ReadLe24(data + 2) && Crc(b->uid) == APP_RefereeCan_ReadLe16(data + 5)) {
      b->id = data[1]; b->fragment = 4;
      BSP_CanFrame_t ack = {.standard_id = 0x123, .dlc = 8}; memcpy(ack.data, data, 8); ack.data[7] = 0xA5; push(ack);
    } else if (b->id && id == APP_ArmorEnum_BusinessId(b->id, 4)) {
      uint32_t gap = tick - b->last_query;
      if (b->last_query && gap > max_gap[i]) max_gap[i] = gap;
      b->last_query = tick;
      BSP_CanFrame_t reply = {.standard_id = APP_ArmorEnum_BusinessId(b->id, 5), .dlc = 8};
      reply.data[0] = 1; push(reply);
    }
  }
  (void)dlc; return 1;
}
/* Test uses the production token/CRC helpers, not a second enumeration engine. */
#include "../Application/Referee/app_armor_enum.c"
static void run(unsigned duration)
{
  unsigned end = tick + duration;
  while (tick < end) {
    tick++; referee_elapsed_ms = tick;
    for (unsigned i = 0; i < 4; i++) {
      Board *b = &boards[i];
      if (!b->present || b->fragment >= 4 || tick < b->due) continue;
      uint32_t token = Token(b->uid);
      BSP_CanFrame_t f = {.standard_id = 0x121, .dlc = 8};
      f.data[0] = b->session; f.data[1] = b->fragment;
      memcpy(f.data + 2, b->uid + b->fragment * 3, 3);
      f.data[5] = (uint8_t)token; f.data[6] = (uint8_t)(token >> 8); f.data[7] = (uint8_t)(token >> 16);
      push(f); b->fragment++; b->due = tick + 2;
    }
    APP_RefereeCan_Task();
  }
}
int main(void)
{
  for (unsigned i = 0; i < 4; i++) {
    for (unsigned j = 0; j < 12; j++) boards[i].uid[j] = (uint8_t)(i * 17 + j);
    boards[i].present = i < 3; boards[i].fragment = 4;
  }
  APP_RefereeCan_Init(); run(2000);
  assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  uint8_t old[3]; for (unsigned i = 0; i < 3; i++) old[i] = boards[i].id;
  boards[3].present = 1; run(8000);
  assert(APP_ArmorEnum_ActiveNodeCount() == 4 && boards[3].id);
  for (unsigned i = 0; i < 3; i++) assert(boards[i].id == old[i]);
  boards[1].present = 0; run(1500);
  assert(APP_ArmorEnum_ActiveNodeCount() == 3);
  run(10000);
  for (unsigned i = 0; i < 4; i++) if (i != 1) {
    assert(boards[i].id); assert(referee_can_monitor.armor_nodes[boards[i].id - 1].online);
    assert(max_gap[i] < 300);
  }
  boards[1].present = 1; boards[1].id = 0; boards[1].fragment = 4; run(8000);
  assert(APP_ArmorEnum_ActiveNodeCount() == 4 && boards[1].id == old[1]);
  puts("production CAN scheduler hot-plug tests passed; healthy polling gaps <300 ms");
}
