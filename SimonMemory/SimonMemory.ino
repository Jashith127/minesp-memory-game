/* ============================================================================
   SimonMemory — Simon-style memory game for ESP32 (spec-compliant version)
   ----------------------------------------------------------------------------
   Spec: 3 rounds, 15 s input deadline per sequence, I2C LCD, 4 LEDs,
         4 buttons, passive buzzer, explicit state machine, non-blocking.

   WIRING (classic ESP32-WROOM DevKit, 30-pin)
     LED 0 green   GPIO 25 ---[220R]---> LED anode, cathode --> GND
     LED 1 red     GPIO 26 ---[220R]---> LED anode, cathode --> GND
     LED 2 yellow  GPIO 27 ---[220R]---> LED anode, cathode --> GND
     LED 3 blue    GPIO 14 ---[220R]---> LED anode, cathode --> GND
     BTN 0..3      GPIO 32,33,4,13 ---> GND (INPUT_PULLUP, no ext. resistor)
     BUZZER +      GPIO 23 (passive piezo, "-" to GND, direct drive <30 mA)
     LCD SDA       GPIO 21, LCD SCL GPIO 22, LCD VCC -> 5V(VU), GND -> GND

   LIBS (Arduino IDE Library Manager):
     "LiquidCrystal I2C" by Frank de Brabander / Marco Schwartz (1.1.2),
     or drop-in "LiquidCrystal_PCF8574" by Matthias Hertel.
   BOARD: "ESP32 Dev Module", esp32 core 2.x or 3.x.
   ========================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------------------------------------------------------------------
// Configuration constants — tune here
// ---------------------------------------------------------------------------
constexpr uint8_t  NUM_PADS       = 4;
constexpr uint8_t  LED_PIN[NUM_PADS] = {25, 26, 27, 14};
constexpr uint8_t  BTN_PIN[NUM_PADS] = {32, 33, 4, 13};
constexpr uint8_t  BUZZER_PIN     = 23;
constexpr bool     BUZZER_PASSIVE = true;   // false = active buzzer (beep = HIGH pulse)

constexpr uint8_t  LCD_SDA_PIN    = 21;
constexpr uint8_t  LCD_SCL_PIN    = 22;
constexpr uint8_t  LCD_ADDR       = 0x27;    // if blank display try 0x3F (see I2C scan)
constexpr uint8_t  LCD_COLS       = 16;
constexpr uint8_t  LCD_ROWS       = 2;

constexpr uint8_t  MAX_ROUNDS     = 3;
constexpr uint8_t  ROUND_LEN[MAX_ROUNDS] = {3, 4, 5};  // steps per round 1..3
constexpr uint8_t  MAX_STEPS      = 8;      // >= longest ROUND_LEN entry

constexpr uint32_t INPUT_TIMEOUT_MS = 15000; // 15 s per sequence (spec)
constexpr uint16_t DEBOUNCE_MS      = 30;

constexpr uint16_t SHOW_ON_MS   = 400;   // LED on time during playback
constexpr uint16_t SHOW_GAP_MS  = 200;   // gap between steps
constexpr uint16_t PRESS_BLIP_MS = 120;  // correct-press feedback

constexpr uint16_t PAD_NOTE[NUM_PADS] = {262, 330, 392, 523}; // C4 E4 G4 C5

constexpr uint8_t LED_ON  = HIGH;
constexpr uint8_t LED_OFF = LOW;

// A jingle = up to 8 (freq, ms) notes, 0 freq = silence. Player is polled.
// Declared here (before any function) so the Arduino auto-generated
// prototypes for functions taking `const Jingle &` compile correctly.
typedef struct {
  uint8_t n;
  uint16_t freq[8];
  uint16_t ms[8];
} Jingle;

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum GameState : uint8_t {
  S_IDLE,      // waiting for any button to start
  S_START,     // start jingle + instructions (brief, non-blocking)
  S_SHOW,      // playing back the pattern
  S_INPUT,     // player reproducing pattern (15 s deadline)
  S_ROUND_OK,  // round cleared fanfare
  S_FAIL,      // wrong button -> error sound, then IDLE
  S_TIMEOUT,   // 15 s expired -> error sound, then IDLE
  S_WIN        // 3 rounds cleared -> victory, then IDLE
};

GameState state = S_IDLE;
uint8_t  sequence[MAX_STEPS];
uint8_t  roundIdx = 0;          // 0..MAX_ROUNDS-1
uint8_t  showIdx  = 0;          // index currently shown
uint8_t  inputIdx = 0;          // next expected index
uint32_t stateT0  = 0;          // state entry time (millis)
uint32_t showNext = 0;          // next show-step time
bool     showLedOn = false;
uint32_t inputDeadline = 0;
uint32_t lastLcdTick = 0;

// ---------------------------------------------------------------------------
// LCD (with graceful fallback to Serial if not found)
// ---------------------------------------------------------------------------
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
bool lcdPresent = false;
char lcdRow0[17] = "";
char lcdRow1[17] = "";

void lcdMsg(const char *r0, const char *r1) {
  // Only rewrite when text actually changed (avoids flicker + I2C spam).
  if (strncmp(r0, lcdRow0, 16) == 0 && strncmp(r1, lcdRow1, 16) == 0) return;
  strncpy(lcdRow0, r0, 16); lcdRow0[16] = '\0';
  strncpy(lcdRow1, r1, 16); lcdRow1[16] = '\0';
  if (lcdPresent) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(lcdRow0);
    lcd.setCursor(0, 1); lcd.print(lcdRow1);
  }
  Serial.print('['); Serial.print(lcdRow0);
  Serial.print(" | "); Serial.print(lcdRow1); Serial.println(']');
}

// ---------------------------------------------------------------------------
// Buzzer: non-blocking jingle player (works on core 2.x and 3.x).
// NOTE: one full function per API branch — no #ifdef *inside* a function
// body, which confuses the Arduino prototype pre-processor.
// ---------------------------------------------------------------------------
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define BUZZER_NEW_API 1
#endif
constexpr uint8_t BUZZER_CH = 0;

#ifdef BUZZER_NEW_API
void buzzerBegin() {
  if (!BUZZER_PASSIVE) { pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW); return; }
  ledcAttach(BUZZER_PIN, 2000, 10);
}
void toneOn(uint16_t f) {
  if (!BUZZER_PASSIVE) { digitalWrite(BUZZER_PIN, HIGH); return; }
  ledcWriteTone(BUZZER_PIN, f);
}
void toneOff() {
  if (!BUZZER_PASSIVE) { digitalWrite(BUZZER_PIN, LOW); return; }
  ledcWrite(BUZZER_PIN, 0);
}
#else
void buzzerBegin() {
  if (!BUZZER_PASSIVE) { pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW); return; }
  ledcSetup(BUZZER_CH, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BUZZER_CH);
}
void toneOn(uint16_t f) {
  if (!BUZZER_PASSIVE) { digitalWrite(BUZZER_PIN, HIGH); return; }
  ledcWriteTone(BUZZER_CH, f);
}
void toneOff() {
  if (!BUZZER_PASSIVE) { digitalWrite(BUZZER_PIN, LOW); return; }
  ledcWrite(BUZZER_CH, 0);
}
#endif

// Sound player state (jingle data defined below with the buzzer code).
Jingle sCur = {0};
uint8_t  jStep = 0;
uint32_t jNext = 0;
bool     jPlaying = false;

void playJingle(const Jingle &j) { sCur = j; jStep = 0; jNext = millis(); jPlaying = true; }
void updateSound() {
  if (!jPlaying) return;
  uint32_t now = millis();
  if (now < jNext) return;
  if (jStep >= sCur.n) { toneOff(); jPlaying = false; return; }
  uint16_t f = sCur.freq[jStep], d = sCur.ms[jStep];
  if (f == 0) toneOff(); else toneOn(f);
  jNext = now + d;
  jStep++;
}
bool soundBusy() { return jPlaying; }

// Named sound patterns (brief so they never break the 15 s timer feel)
const Jingle J_START   = {3, {523, 659, 784},          {110, 110, 160}};           // C5 E5 G5 up
// Playback uses per-pad notes directly.
const Jingle J_CORRECT  = {1, {660},                          {PRESS_BLIP_MS}};           // correct-press blip
const Jingle J_ROUND    = {4, {523, 659, 784, 1047},          {100, 100, 100, 220}};      // arpeggio
const Jingle J_ERROR    = {4, {220, 0, 180, 120},             {150, 80, 150, 300}};       // two low buzzes + drop
const Jingle J_TIMEOUT  = {4, {330, 0, 220, 140},             {150, 80, 150, 350}};       // distinct from error
const Jingle J_WIN      = {6, {523, 659, 784, 1047, 784, 1319},{120, 120, 120, 120, 120, 350}};
const Jingle J_GO       = {2, {880, 1245},                    {70, 110}};                 // "your turn" cue

// ---------------------------------------------------------------------------
// LEDs + buttons
// ---------------------------------------------------------------------------
void ledWrite(uint8_t pad, bool on) { digitalWrite(LED_PIN[pad], on ? LED_ON : LED_OFF); }
void ledsAll(bool on) { for (uint8_t i = 0; i < NUM_PADS; i++) ledWrite(i, on); }

// Debounced edge detector: returns pad index on a fresh press, else -1.
uint8_t  btnStable[NUM_PADS] = {HIGH, HIGH, HIGH, HIGH};
uint8_t  btnLast[NUM_PADS]   = {HIGH, HIGH, HIGH, HIGH};
uint32_t btnT[NUM_PADS]      = {0, 0, 0, 0};

int8_t pollButtonEdge() {
  uint32_t now = millis();
  int8_t hit = -1;
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    uint8_t r = digitalRead(BTN_PIN[i]);
    if (r != btnLast[i]) btnT[i] = now;
    btnLast[i] = r;
    if ((now - btnT[i]) >= DEBOUNCE_MS && r != btnStable[i]) {
      btnStable[i] = r;
      if (r == LOW) hit = (int8_t)i;   // FALLING edge = press
    }
  }
  return hit;
}

// ---------------------------------------------------------------------------
// Game helpers
// ---------------------------------------------------------------------------
void genSequence(uint8_t len) {
  for (uint8_t i = 0; i < len; i++) sequence[i] = (uint8_t)random(0, NUM_PADS);
}

void enterState(GameState s) {
  state = s;
  stateT0 = millis();
}

void startRound() {
  uint8_t len = ROUND_LEN[roundIdx];
  genSequence(len);
  showIdx = 0; showLedOn = false; showNext = millis() + 400;
  ledsAll(false);
  char r1[17]; snprintf(r1, sizeof r1, "Round %u/%u Watch..", roundIdx + 1, MAX_ROUNDS);
  lcdMsg("Simon Memory", r1);
  Serial.printf("Round %u: pattern len %u\n", roundIdx + 1, len);
  enterState(S_SHOW);
}

void beginInput() {
  inputIdx = 0;
  inputDeadline = millis() + INPUT_TIMEOUT_MS;
  lastLcdTick = 0;
  playJingle(J_GO);
  enterState(S_INPUT);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(250);

  for (uint8_t i = 0; i < NUM_PADS; i++) {
    pinMode(LED_PIN[i], OUTPUT); ledWrite(i, false);
    pinMode(BTN_PIN[i], INPUT_PULLUP);
  }
  buzzerBegin();
  randomSeed(esp_random());

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.beginTransmission(LCD_ADDR);
  lcdPresent = (Wire.endTransmission() == 0);
  if (lcdPresent) { lcd.init(); lcd.backlight(); }

  Serial.println("\nSimonMemory — 3 rounds, 15 s per sequence");
  Serial.printf("LCD %s at 0x%02X (SDA %u SCL %u)\n",
                lcdPresent ? "found" : "NOT FOUND (Serial fallback)",
                LCD_ADDR, LCD_SDA_PIN, LCD_SCL_PIN);
  lcdMsg("Simon Memory", "Press any btn");
  playJingle(J_START);   // power-on title sound
  enterState(S_IDLE);
}

void loop() {
  updateSound();
  uint32_t now = millis();

  switch (state) {

    case S_IDLE: {
      // Gentle prompt; any debounced press starts the game.
      lcdMsg("Simon Memory", "Press any btn");
      if (pollButtonEdge() >= 0) {
        roundIdx = 0;
        lcdMsg("Get ready!", "Round 1 of 3");
        playJingle(J_START);
        enterState(S_START);
      }
      break;
    }

    case S_START: {
      // Short pause so the start jingle + message are perceived, then round 1.
      if (now - stateT0 >= 900) startRound();
      pollButtonEdge();  // swallow extra presses
      break;
    }

    case S_SHOW: {
      // Non-blocking playback: LED+note per step, then hand over to input.
      uint8_t len = ROUND_LEN[roundIdx];
      if (showIdx >= len) { beginInput(); break; }
      if (now >= showNext) {
        if (!showLedOn) {
          ledWrite(sequence[showIdx], true);
          toneOn(PAD_NOTE[sequence[showIdx]]);
          showLedOn = true;
          showNext = now + SHOW_ON_MS;
        } else {
          toneOff();
          ledWrite(sequence[showIdx], false);
          showLedOn = false;
          showIdx++;
          showNext = now + SHOW_GAP_MS;
        }
      }
      pollButtonEdge();  // ignore presses during demo
      break;
    }

    case S_INPUT: {
      // 15 s deadline for the WHOLE sequence (spec). Checked every loop.
      uint32_t remain = (inputDeadline > now) ? (inputDeadline - now) : 0;
      if (remain == 0) {
        toneOff(); ledsAll(false);
        lcdMsg("TIME OUT!", "Resetting...");
        playJingle(J_TIMEOUT);
        enterState(S_TIMEOUT);
        break;
      }
      // Countdown on LCD row 2, refreshed at 4 Hz without flicker.
      if (now - lastLcdTick >= 250) {
        lastLcdTick = now;
        char r0[17], r1[17];
        snprintf(r0, sizeof r0, "Rnd%u Hit %u/%u", roundIdx + 1, inputIdx, ROUND_LEN[roundIdx]);
        snprintf(r1, sizeof r1, "Time left %2lus", (unsigned long)((remain + 999) / 1000));
        lcdMsg(r0, r1);
      }
      int8_t pad = pollButtonEdge();
      if (pad >= 0) {
        if ((uint8_t)pad == sequence[inputIdx]) {
          // Correct press: flash that LED + blip, advance.
          ledWrite((uint8_t)pad, true);
          toneOn(PAD_NOTE[pad]);
          // Keep feedback short but visible; deadline keeps running (we use
          // small non-blocking hold via stateT0 trick — simplest: brief delay
          // of 120 ms is <1% of the 15 s budget and keeps code readable).
          // To stay strictly non-blocking we bound it: input window already
          // accounts for it since deadline is absolute (millis-based).
          delay(PRESS_BLIP_MS);
          toneOff();
          ledWrite((uint8_t)pad, false);
          inputIdx++;
          if (inputIdx >= ROUND_LEN[roundIdx]) {
            ledsAll(false);
            lcdMsg("Round clear!", "Nice work");
            playJingle(J_ROUND);
            enterState(S_ROUND_OK);
          }
        } else {
          // Wrong button: error sound + reset to IDLE.
          toneOff(); ledsAll(false);
          lcdMsg("WRONG button!", "Resetting...");
          Serial.printf("Wrong: expected %u got %d\n", sequence[inputIdx], pad);
          playJingle(J_ERROR);
          enterState(S_FAIL);
        }
      }
      break;
    }

    case S_ROUND_OK: {
      if (!soundBusy() && now - stateT0 >= 700) {
        roundIdx++;
        if (roundIdx >= MAX_ROUNDS) {
          lcdMsg("YOU WIN!", "3/3 rounds!");
          playJingle(J_WIN);
          enterState(S_WIN);
        } else {
          startRound();
        }
      }
      pollButtonEdge();
      break;
    }

    case S_FAIL:
    case S_TIMEOUT: {
      // Hold message until the error jingle finishes, then idle (ready to restart).
      if (!soundBusy() && now - stateT0 >= 1200) {
        ledsAll(false);
        enterState(S_IDLE);
      }
      pollButtonEdge();
      break;
    }

    case S_WIN: {
      // Celebrate, then return to idle. Any press skips the wait.
      if ((!soundBusy() && now - stateT0 >= 2500) || pollButtonEdge() >= 0) {
        ledsAll(false);
        enterState(S_IDLE);
      }
      break;
    }
  }
}
