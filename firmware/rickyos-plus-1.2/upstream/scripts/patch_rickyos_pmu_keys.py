"""Let the side key wake Read Pico from Standby's light sleep.

The CW32L010 PMU queues key events (KEY_DOWN, KEY_UP, KEY_SHORT, ...) in an
eight-deep FIFO and holds its interrupt line while any are pending. That line
reaches the host through the FCA9555, whose INT# (GPIO41) is the light-sleep
wake. The SDK reads the key as a level and never acknowledges the events, so
after a few presses the FIFO is full, the PMU line stays asserted, the expander
sees no further change, and a press no longer ends the sleep: Standby only woke
on its minute timer and missed the press. The reference firmware drains the
FIFO around every lock-screen sleep (main/sleep.c, read_pico_pmu.c
`read_pico_pmu_take_key_wakeup`); this exposes the same drain.

Like patch_rickyos_sdcard.py: a bounded mechanical rewrite, re-runnable, and the
build fails if the SDK source no longer matches instead of silently skipping it.
"""
from pathlib import Path

MARK = 'RICKYOS_PMU_KEY_EVENTS'

OLD_DECL = '''bool ioeIntAsserted();
void clearIoeInt();
'''
V1_DECL = '''bool ioeIntAsserted();
void clearIoeInt();

// RICKYOS_PMU_KEY_EVENTS: acknowledge every queued PMU event so its interrupt
// line releases and the next press can pull INT# again. True when a press
// (KEY_DOWN or KEY_SHORT) was among them; keyDown is the key level right after.
bool takePmuKeyPress(bool& keyDown);
'''
V2_DECL = '''bool ioeIntAsserted();
void clearIoeInt();
// RICKYOS_PMU_KEY_EVENTS: acknowledge every queued PMU event so its interrupt
// line releases and the next press can pull INT# again. True when a press
// (KEY_DOWN or KEY_SHORT) newer than event id newerThan was among them (0: any);
// keyDown is the key level right after.
bool takePmuKeyPress(bool& keyDown, uint16_t newerThan = 0);
// STATUS.last_event_id from the latest poll: the id of the newest event the PMU
// has raised, so a later takePmuKeyPress can tell new presses from old ones.
uint16_t pmuLastEventId();
'''
NEW_DECL = '''bool ioeIntAsserted();
void clearIoeInt();
// RICKYOS_PMU_KEY_EVENTS: acknowledge every queued PMU event so its interrupt
// line releases and the next press can pull INT# again. True when a press
// (KEY_DOWN or KEY_SHORT) newer than event id newerThan was among them (0: any);
// keyDown is the key level right after.
bool takePmuKeyPress(bool& keyDown, uint16_t newerThan = 0);
// STATUS.last_event_id from the latest poll: the id of the newest event the PMU
// has raised, so a later takePmuKeyPress can tell new presses from old ones.
uint16_t pmuLastEventId();
// Type of the last event takePmuKeyPress acknowledged (0: none yet), for logs.
uint8_t pmuLastEventType();
'''

OLD_DEF = '''bool sdCardPresent() {
  // Slot CD is active-low'''
V1_DEF = '''// RICKYOS_PMU_KEY_EVENTS (read_pico_pmu.c `read_pico_pmu_take_key_wakeup`).
bool takePmuKeyPress(bool& keyDown) {
  constexpr uint8_t kKeyDown = 0x01;   // PMU_EVT_KEY_DOWN
  constexpr uint8_t kKeyShort = 0x03;  // PMU_EVT_KEY_SHORT
  constexpr int kFifoDepth = 8;        // PMU_EVENT_FIFO_DEPTH
  bool pressed = false;
  keyDown = false;
  if (!g_pmuPresent) return false;
  for (int i = 0; i < kFifoDepth; ++i) {
    if (!pmuPoll()) break;
    keyDown = g_pmuKeyDown;
    if (g_pmuPendingEvents == 0) break;
    uint16_t id = 0;
    uint8_t type = 0;
    if (!pmuPeekEvent(id, type)) break;
    if (type == kKeyDown || type == kKeyShort) pressed = true;
    if (!pmuEventAck(id)) break;
    delay(10);  // the PMU updates STATUS after an ACK (read_pico_pmu_event_ack)
  }
  return pressed;
}

bool sdCardPresent() {
  // Slot CD is active-low'''
V2_DEF = '''// RICKYOS_PMU_KEY_EVENTS (read_pico_pmu.c `read_pico_pmu_take_key_wakeup`).
// The queue is read until EVENT_PEEK reports it empty: STATUS.pending_events can
// read 0 while an old event is still queued, and that event, read after the next
// light sleep, looked like a fresh press and closed Standby by itself.
bool takePmuKeyPress(bool& keyDown, uint16_t newerThan) {
  constexpr uint8_t kKeyDown = 0x01;   // PMU_EVT_KEY_DOWN
  constexpr uint8_t kKeyShort = 0x03;  // PMU_EVT_KEY_SHORT
  constexpr int kFifoDepth = 8;        // PMU_EVENT_FIFO_DEPTH
  bool pressed = false;
  keyDown = false;
  if (!g_pmuPresent) return false;
  if (pmuPoll()) keyDown = g_pmuKeyDown;
  for (int i = 0; i < kFifoDepth; ++i) {
    uint16_t id = 0;
    uint8_t type = 0;
    if (!pmuPeekEvent(id, type)) break;  // empty (id 0) or unreadable
    const bool fresh = newerThan == 0 || static_cast<int16_t>(id - newerThan) > 0;
    if (fresh && (type == kKeyDown || type == kKeyShort)) pressed = true;
    if (!pmuEventAck(id)) break;
    delay(10);  // the PMU updates STATUS after an ACK (read_pico_pmu_event_ack)
  }
  if (pmuPoll()) keyDown = g_pmuKeyDown;
  return pressed;
}
uint16_t pmuLastEventId() { return g_pmuLastEventId.load(); }
bool sdCardPresent() {
  // Slot CD is active-low'''
NEW_DEF = '''// RICKYOS_PMU_KEY_EVENTS (read_pico_pmu.c `read_pico_pmu_take_key_wakeup`).
// The queue is read until EVENT_PEEK reports it empty: STATUS.pending_events can
// read 0 while an old event is still queued, and that event, read after the next
// light sleep, looked like a fresh press and closed Standby by itself.
uint8_t g_rickyLastEventType = 0;
bool takePmuKeyPress(bool& keyDown, uint16_t newerThan) {
  constexpr uint8_t kKeyDown = 0x01;   // PMU_EVT_KEY_DOWN
  constexpr uint8_t kKeyShort = 0x03;  // PMU_EVT_KEY_SHORT
  constexpr int kFifoDepth = 8;        // PMU_EVENT_FIFO_DEPTH
  bool pressed = false;
  keyDown = false;
  if (!g_pmuPresent) return false;
  if (pmuPoll()) keyDown = g_pmuKeyDown;
  for (int i = 0; i < kFifoDepth; ++i) {
    uint16_t id = 0;
    uint8_t type = 0;
    if (!pmuPeekEvent(id, type)) break;  // empty (id 0) or unreadable
    g_rickyLastEventType = type;
    const bool fresh = newerThan == 0 || static_cast<int16_t>(id - newerThan) > 0;
    if (fresh && (type == kKeyDown || type == kKeyShort)) pressed = true;
    if (!pmuEventAck(id)) break;
    delay(10);  // the PMU updates STATUS after an ACK (read_pico_pmu_event_ack)
  }
  if (pmuPoll()) keyDown = g_pmuKeyDown;
  return pressed;
}
uint16_t pmuLastEventId() { return g_pmuLastEventId.load(); }
uint8_t pmuLastEventType() { return g_rickyLastEventType; }
bool sdCardPresent() {
  // Slot CD is active-low'''


def patch_text(source, old, new, previous=(), mark=MARK):
    if new in source:
        return source
    # A tree patched by an earlier RickyOS build upgrades in place.
    for earlier in previous:
        if source.count(earlier) == 1:
            return source.replace(earlier, new)
    if source.count(old) != 1:
        raise RuntimeError('SDK Read Pico board source changed; review the RickyOS patch (%s) before building.' % mark)
    return source.replace(old, new)


def apply(project_dir):
    board = Path(project_dir) / 'freeink-sdk/libs/hardware/BoardReadPico'
    for rel, old, new, previous in (('include/BoardReadPico.h', OLD_DECL, NEW_DECL, (V2_DECL, V1_DECL)),
                                    ('src/BoardReadPico.cpp', OLD_DEF, NEW_DEF, (V2_DEF, V1_DEF))):
        path = board / rel
        source = path.read_text()
        patched = patch_text(source, old, new, previous)
        if patched != source:
            path.write_text(patched)


if 'Import' in globals():
    Import('env')
    if env.subst('$PIOENV') == 'rickyos_readpico':
        apply(env.subst('$PROJECT_DIR'))
elif __name__ == '__main__':
    apply(Path(__file__).resolve().parents[1])
