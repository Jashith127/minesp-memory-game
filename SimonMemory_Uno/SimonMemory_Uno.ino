/* ============================================================================
   SimonMemory_Uno — Simon-style memory game for ARDUINO UNO (R3/SMD)
   ----------------------------------------------------------------------------
   Spec: 3 rounds, 15 s input deadline per sequence, I2C LCD, 4 LEDs,
         4 buttons, passive buzzer, explicit state machine, non-blocking.

   WIRING (Arduino Uno, SMD or DIP — same pinout)
     LED 0 green   D5 --- 220 ohm --- GND   (anode toward D5, cathode to GND)
     LED 1 red     D6 --- 220 ohm --- GND
     LED 2 yellow  D7 --- 220 ohm --- GND
     LED 3 blue    D8 --- 220 ohm --- GND
     BTN 0..3      D2, D3, D4, D9 ---> GND (INPUT_PULLUP, no ext. resistor)
     BUZZER +      D10 (passive piezo, "-" to GND)
     LCD SDA       A4, LCD SCL A5 (fixed Uno I2C pins), VCC -> 5V, GND -> GND
     (The extra SDA/SCL pins near the USB port are the same net as A4/A5.)

   LIBS (Arduino IDE Library Manager):
     "LiquidCrystal I2C" by Frank de Brabander / Marco Schwartz (1.1.2).
   BOARD: "Arduino Uno".  Baud 115200 or 9600 (see SERIAL_BAUD below).
   ========================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------------------------------------------------------------------
// Configuration constants — tune here
// ---------------------------------------------------------------------------
constexpr uint8_t  NUM_PADS       = 4;
constexpr uint8_t  LED_PIN[NUM_PADS] = {5, 6, 7, 8};
constexpr uint8_t  BTN_PIN[NUM_PADS] = {2, 3, 4, 9};
constexpr uint8_t  BUZZER_PIN     = 10;
constexpr bool     BUZZER_PASSIVE = true;   // false = active buzzer (beep = HIGH pulse)
constexpr bool     BUZZER_ENABLED = false;  // true once the buzzer is wired up

constexpr uint8_t  LCD_ADDR       = 0x27;    // if blank display try 0x3F (see I2C scan)
constexpr uint8_t  LCD_COLS       = 16;
constexpr uint8_t  LCD_ROWS       = 2;
// Uno I2C pins are fixed in hardware: SDA = A4, SCL = A5. Wire.begin() takes
// no arguments. A0 is left floating on purpose (random seed noise source).
constexpr uint8_t  NOISE_PIN      = A0;

constexpr uint32_t SERIAL_BAUD    = 115200;

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
// Buzzer: Arduino tone() is already non-blocking (hardware Timer2), so the
// polled jingle player below just schedules note changes via millis().
// ---------------------------------------------------------------------------
void buzzerBegin() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}
void toneOn(uint16_t f) {
  if (!BUZZER_ENABLED) return;             // buzzer not wired yet — stay silent
  if (!BUZZER_PASSIVE) { digitalWrite(BUZZER_PIN, HIGH); return; }
  tone(BUZZER_PIN, f);
}
void toneOff() {
  if (!BUZZER_ENABLED) return;
  if (!BUZZER_PASSIVE) { digitalWrite(BUZZER_PIN, LOW); return; }
  noTone(BUZZER_PIN);
}

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
const Jingle J_CORRECT  = {1, {660},                          {PRESS_BLIP_MS}};   // correct-press blip
const Jingle J_ROUND    = {4, {523, 659, 784, 1047},          {100, 100, 100, 220}}; // arpeggio
const Jingle J_ERROR    = {4, {220, 0, 180, 120},             {150, 80, 150, 300}}; // two low buzzes + drop
const Jingle J_TIMEOUT  = {4, {330, 0, 220, 140},             {150, 80, 150, 350}}; // distinct from error
const Jingle J_WIN      = {6, {523, 659, 784, 1047, 784, 1319},{120, 120, 120, 120, 120, 350}};
const Jingle J_GO       = {2, {880, 1245},                    {70, 110}};          // "your turn" cue

// ---------------------------------------------------------------------------
// LEDs + buttons
// ---------------------------------------------------------------------------
void ledWrite(uint8_t pad, bool on) { digitalWrite(LED_PIN[pad], on ? LED_ON : LED_OFF); }
void ledsAll(bool on) { for (uint8_t i = 0; i < NUM_PADS; i++) ledWrite(i, on); }

// All-LED simultaneous flash = visible right/wrong signal (buzzer is muted).
// Blocking, so only call from transitions with no active input deadline
// (round-clear / fail / timeout / win). Always ends with LEDs off.
void flashAll(uint8_t times, uint16_t onMs, uint16_t offMs) {
  for (uint8_t k = 0; k < times; k++) {
    ledsAll(true); delay(onMs);
    ledsAll(false);
    if (k + 1 < times) delay(offMs);
  }
}

// Debounced edge detector: returns pad index on a fresh press, else -1.
uint8_t  btnStable[NUM_PADS] = {HIGH, HIGH, HIGH, HIGH};
uint8_t  btnLast[NUM_PADS]   = {HIGH, HIGH, HIGH, HIGH};
uint32_t btnT[NUM_PADS]      = {0, 0, 0, 0};

int8_t pollButtonEdge() {
  uint32_t now = millis();
  int8_t hit = -1;
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    uint8_t r = digitalRead(BTN_PIN[i]);
    if (r != btnLast[i]) { btnT[i] = now; btnLast[i] = r; }
    // Press registers after only 15 ms LOW (catches quick taps);
    // release needs 30 ms HIGH (debounce). Asymmetric on purpose.
    if (r == LOW && btnStable[i] == HIGH && (now - btnT[i]) >= 15) {
      btnStable[i] = LOW;
      hit = (int8_t)i;   // FALLING edge = press
    } else if (r == HIGH && btnStable[i] == LOW && (now - btnT[i]) >= DEBOUNCE_MS) {
      btnStable[i] = HIGH;
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
  char r1[17]; snprintf(r1, sizeof r1, "Round %u Watch..", roundIdx + 1);
  lcdMsg("Simon Memory", r1);
  Serial.print("Round "); Serial.print(roundIdx + 1);
  Serial.print(": pattern len "); Serial.println(len);
  enterState(S_SHOW);
}

void beginInput() {
  flashAll(1, 120, 0);   // visual "your turn" cue (buzzer is muted)
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
  Serial.begin(SERIAL_BAUD);
  delay(250);

  for (uint8_t i = 0; i < NUM_PADS; i++) {
    pinMode(LED_PIN[i], OUTPUT); ledWrite(i, false);
    pinMode(BTN_PIN[i], INPUT_PULLUP);
  }
  buzzerBegin();
  randomSeed(analogRead(NOISE_PIN));  // A0 floating = noise source

  Wire.begin();  // Uno I2C pins are fixed: SDA = A4, SCL = A5
  Serial.println();
  Serial.println("SimonMemory_Uno - 3 rounds, 15 s per sequence");
  Serial.println("I2C scan:");
  uint8_t foundAddr = 0;
  for (uint8_t a = 8; a < 120; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("  found 0x"); Serial.println(a, HEX);
      // Any PCF8574-family address (0x20-0x3F) is treated as the LCD
      // backpack. Normal modules answer at 0x27 (or 0x3F for A-variant).
      if (a >= 0x20 && a < 0x40 && foundAddr == 0) foundAddr = a;
    }
  }
  if (foundAddr != 0) {
    lcd = LiquidCrystal_I2C(foundAddr, LCD_COLS, LCD_ROWS);
    lcd.init(); lcd.backlight();
    lcdPresent = true;
    Serial.print("LCD ok at 0x"); Serial.println(foundAddr, HEX);
  } else {
    Serial.println("NO LCD (Serial fallback). Check SDA=A4 SCL=A5 5V GND.");
  }
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
        toneOff(); flashAll(2, 300, 150); ledsAll(false);
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
        snprintf(r1, sizeof r1, "Time left %lus", (unsigned long)((remain + 999) / 1000));
        lcdMsg(r0, r1);
      }
      int8_t pad = pollButtonEdge();
      if (pad >= 0) {
        if ((uint8_t)pad == sequence[inputIdx]) {
          // Correct press: flash that LED + blip, advance.
          Serial.print("ok pad "); Serial.println(pad);
          ledWrite((uint8_t)pad, true);
          toneOn(PAD_NOTE[pad]);
          // Brief, bounded feedback. The deadline is absolute (millis-based)
          // so this 120 ms cannot stretch the 15 s input window.
          delay(PRESS_BLIP_MS);
          toneOff();
          ledWrite((uint8_t)pad, false);
          inputIdx++;
          if (inputIdx >= ROUND_LEN[roundIdx]) {
            flashAll(2, 120, 80); ledsAll(false);
            lcdMsg("Round clear!", "Nice work");
            playJingle(J_ROUND);
            enterState(S_ROUND_OK);
          }
        } else {
          // Wrong button: flash all LEDs + error sound, reset to IDLE.
          toneOff(); flashAll(3, 150, 100); ledsAll(false);
          lcdMsg("WRONG button!", "Resetting...");
          Serial.print("Wrong: expected "); Serial.print(sequence[inputIdx]);
          Serial.print(" got "); Serial.println(pad);
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
          flashAll(4, 120, 100); ledsAll(false);
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
