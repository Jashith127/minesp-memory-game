/* ============================================================================
   MinESP — a 4-pad memory game (Simon style) for ESP32
   ----------------------------------------------------------------------------
   Four LEDs show a pattern that grows by one step every round. Repeat it with
   the four buttons. A passive buzzer plays a note per pad, a little arpeggio
   when you clear a round, and a sad buzz when you miss.

   WIRING  (buttons switch to GND, internal pull-ups are used)
     LED 0 green   GPIO 25  --- 220 ohm --- GND
     LED 1 red     GPIO 26  --- 220 ohm --- GND
     LED 2 yellow  GPIO 27  --- 220 ohm --- GND
     LED 3 blue    GPIO 14  --- 220 ohm --- GND

     BTN 0 green   GPIO 32  --- GND
     BTN 1 red     GPIO 33  --- GND
     BTN 2 yellow  GPIO 4   --- GND
     BTN 3 blue    GPIO 13  --- GND

     BUZZER +      GPIO 23      (passive buzzer, "-" to GND)

   Arduino IDE: board "ESP32 Dev Module". Works with ESP32 Arduino core 2.x
   and 3.x (the LEDC calls are version-guarded below).
   ========================================================================== */

#include <Preferences.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// Hardware map — change these if you wired it differently
// ---------------------------------------------------------------------------
constexpr uint8_t NUM_PADS = 4;

constexpr uint8_t LED_PIN[NUM_PADS] = {25, 26, 27, 14};
constexpr uint8_t BTN_PIN[NUM_PADS] = {32, 33, 4, 13};
constexpr uint8_t BUZZER_PIN = 23;

// Flip these two if your LEDs are wired pin -> LED -> resistor -> 3V3 (active low)
constexpr uint8_t LED_ON  = HIGH;
constexpr uint8_t LED_OFF = LOW;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
// One note per pad: C4, E4, G4, C5
constexpr uint16_t PAD_NOTE[NUM_PADS] = {262, 330, 392, 523};

constexpr uint16_t SHOW_ON_MS      = 320;  // how long each pad lights up
constexpr uint16_t MIN_SHOW_ON_MS  = 170;  // fastest it gets at high levels
constexpr uint16_t SHOW_SPEEDUP    = 8;    // ms shaved off per level
constexpr uint16_t SHOW_GAP_MS     = 150;  // silence between steps
constexpr uint16_t PRESS_BLIP_MS   = 90;   // feedback when you press correctly

constexpr uint16_t INPUT_BASE_MS   = 5000; // time allowed for one press
constexpr uint16_t INPUT_PER_LEVEL_MS = 400;
constexpr uint16_t INPUT_MAX_MS    = 9000;

constexpr uint8_t  MAX_STEPS       = 40;   // sequence buffer size
constexpr uint8_t  DEBOUNCE_MS     = 25;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
uint8_t  sequence[MAX_STEPS];
uint8_t  seqLen = 0;          // current pattern length (= current level)
uint16_t bestLevel = 0;       // best number of levels cleared (saved in NVS)

Preferences prefs;

// ---------------------------------------------------------------------------
// Buzzer (LEDC PWM square wave), works on core 2.x and 3.x
// ---------------------------------------------------------------------------
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define BUZZER_NEW_API 1
#endif

constexpr uint8_t BUZZER_CHANNEL = 0;

void buzzerBegin() {
#ifdef BUZZER_NEW_API
  ledcAttach(BUZZER_PIN, 2000, 10);
#else
  ledcSetup(BUZZER_CHANNEL, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
#endif
}

void toneOn(uint16_t freq) {
#ifdef BUZZER_NEW_API
  ledcWriteTone(BUZZER_PIN, freq);
#else
  ledcWriteTone(BUZZER_CHANNEL, freq);
#endif
}

void toneOff() {
#ifdef BUZZER_NEW_API
  ledcWrite(BUZZER_PIN, 0);
#else
  ledcWrite(BUZZER_CHANNEL, 0);
#endif
}

void beep(uint16_t freq, uint16_t ms) {
  toneOn(freq);
  delay(ms);
  toneOff();
}

// ---------------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------------
void ledWrite(uint8_t pad, bool on) {
  digitalWrite(LED_PIN[pad], on ? LED_ON : LED_OFF);
}

void ledsAll(bool on) {
  for (uint8_t i = 0; i < NUM_PADS; i++) ledWrite(i, on);
}

void flashPad(uint8_t pad, uint16_t ms) {
  ledWrite(pad, true);
  toneOn(PAD_NOTE[pad]);
  delay(ms);
  toneOff();
  ledWrite(pad, false);
}

// ---------------------------------------------------------------------------
// Button helpers
// ---------------------------------------------------------------------------
// Returns the pad being held, or -1. Includes a short debounce.
int8_t pollPress() {
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    if (digitalRead(BTN_PIN[i]) == LOW) {
      delay(DEBOUNCE_MS / 2);
      if (digitalRead(BTN_PIN[i]) == LOW) return (int8_t)i;
    }
  }
  return -1;
}

void waitRelease(uint8_t pad) {
  uint32_t held = millis();
  while (digitalRead(BTN_PIN[pad]) == LOW && (millis() - held) < 5000) delay(5);
}

// Waits for a press and its release. Returns pad index or -1 on timeout.
int8_t waitPress(uint16_t timeoutMs) {
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    int8_t pad = pollPress();
    if (pad >= 0) {
      waitRelease((uint8_t)pad);
      return pad;
    }
    delay(2);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// High score (stored in flash via NVS)
// ---------------------------------------------------------------------------
void saveBest(uint16_t value) {
  bestLevel = value;
  prefs.putUShort("best", value);
}

// ---------------------------------------------------------------------------
// Sound effects
// ---------------------------------------------------------------------------
void startupJingle() {
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    ledWrite(i, true);
    toneOn(PAD_NOTE[i]);
    delay(120);
    toneOff();
    ledWrite(i, false);
    delay(35);
  }
}

void readyCue() {
  beep(880, 60);
  delay(60);
  beep(1245, 80);
  delay(120);
}

void showFailure() {
  for (uint8_t k = 0; k < 3; k++) {
    ledsAll(true);
    toneOn(150);
    delay(180);
    toneOff();
    ledsAll(false);
    delay(110);
  }
  beep(90, 350);
}

void roundComplete() {
  const uint16_t arp[4] = {523, 659, 784, 1047};
  for (uint8_t k = 0; k < 4; k++) {
    ledsAll(true);
    toneOn(arp[k]);
    delay(90);
    toneOff();
    ledsAll(false);
    delay(35);
  }
  delay(180);
}

// ---------------------------------------------------------------------------
// Idle: chase the LEDs until somebody presses a button
// ---------------------------------------------------------------------------
void idleUntilPress() {
  Serial.println("Press any button to start");
  int8_t i = 0;
  for (;;) {
    ledWrite((uint8_t)i, true);
    uint32_t t = millis();
    int8_t pad = -1;
    while ((millis() - t) < 160) {
      pad = pollPress();
      if (pad >= 0) break;
      delay(3);
    }
    ledWrite((uint8_t)i, false);

    if (pad >= 0) {
      waitRelease((uint8_t)pad);
      beep(660, 70);
      return;
    }
    i = (i + 1) % NUM_PADS;
    delay(90);
  }
}

// ---------------------------------------------------------------------------
// Pattern
// ---------------------------------------------------------------------------
void addStep() {
  uint8_t next;
  do {
    next = (uint8_t)random(0, NUM_PADS);
  } while (seqLen > 0 && next == sequence[seqLen - 1]);  // no boring repeats
  sequence[seqLen++] = next;
}

uint16_t showOnTime() {
  uint16_t cut = (seqLen > 3) ? (uint16_t)(seqLen - 3) * SHOW_SPEEDUP : 0;
  uint16_t fastestCut = SHOW_ON_MS - MIN_SHOW_ON_MS;
  if (cut >= fastestCut) return MIN_SHOW_ON_MS;
  return SHOW_ON_MS - cut;
}

void showSequence() {
  uint16_t on = showOnTime();
  for (uint8_t i = 0; i < seqLen; i++) {
    flashPad(sequence[i], on);
    delay(SHOW_GAP_MS);
  }
}

// ---------------------------------------------------------------------------
// Player input: true if the whole pattern was repeated correctly
// ---------------------------------------------------------------------------
bool readSequence() {
  uint16_t timeout = INPUT_BASE_MS + (uint16_t)seqLen * INPUT_PER_LEVEL_MS;
  if (timeout > INPUT_MAX_MS) timeout = INPUT_MAX_MS;

  for (uint8_t i = 0; i < seqLen; i++) {
    int8_t pad = waitPress(timeout);
    if (pad < 0) {
      Serial.println("  ...out of time");
      return false;
    }

    bool correct = (pad == sequence[i]);
    ledWrite((uint8_t)pad, true);
    if (correct) {
      toneOn(PAD_NOTE[pad]);
      delay(PRESS_BLIP_MS);
      toneOff();
      ledWrite((uint8_t)pad, false);
    } else {
      ledWrite((uint8_t)pad, false);
      Serial.printf("  ...wrong pad (expected %u, pressed %u)\n",
                    sequence[i], (uint8_t)pad);
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// One game: keep adding steps until the player misses
// ---------------------------------------------------------------------------
void playGame() {
  seqLen = 0;
  bool won = false;

  for (;;) {
    addStep();
    Serial.printf("\nLevel %u - watch...\n", seqLen);
    delay(300);

    showSequence();
    readyCue();

    if (!readSequence()) break;

    Serial.println("  nice!");
    if (seqLen > bestLevel) {
      saveBest(seqLen);
      Serial.printf("  * new best: %u levels cleared *\n", seqLen);
    }
    roundComplete();

    if (seqLen >= MAX_STEPS) {
      won = true;
      break;
    }
  }

  if (won) {
    Serial.println("MAX LEVEL - you beat the game!");
    roundComplete();
    roundComplete();
  } else {
    showFailure();
  }

  uint16_t cleared = won ? seqLen : ((seqLen > 0) ? (uint16_t)(seqLen - 1) : 0);
  Serial.printf("Cleared %u levels (best %u).\n\n", cleared, bestLevel);
  ledsAll(false);
  delay(700);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(250);

  for (uint8_t i = 0; i < NUM_PADS; i++) {
    pinMode(LED_PIN[i], OUTPUT);
    ledWrite(i, false);
    pinMode(BTN_PIN[i], INPUT_PULLUP);
  }

  buzzerBegin();
  randomSeed(esp_random());

  if (!prefs.begin("minesp", false)) {
    Serial.println("NVS unavailable - high score will not persist");
  }
  bestLevel = prefs.getUShort("best", 0);

  Serial.println();
  Serial.println("MinESP - 4-pad memory game");
  Serial.printf("Best so far: %u levels cleared\n", bestLevel);
  Serial.println("---------------------------------");

  startupJingle();
}

void loop() {
  idleUntilPress();
  playGame();
  delay(600);
}
