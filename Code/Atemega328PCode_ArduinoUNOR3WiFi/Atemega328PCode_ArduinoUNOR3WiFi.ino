/*  ------ UNO (ATmega328P) CODE ------
    - Keypad input 4x4 :
      Keypad Pin : ROWS 2, 3, 4, 5; COLS 6, 7, A0, A1
      Setpoint PH & ON Duration : A
      Setpoint PPM & OFF Duration : B
      Backspace : C
      Scroll Display & Cancel Input Mode : D
    - LCD I2C 20x4, 3 Display Interface :
      LCD Pin : SDA A4; SCL A5
      LCD Address : 0x27
    - PH (A2) and PPM (A3) read
    - Valve 1..4 on pins 8..11 (active LOW relay)
    - Store setpoints & durations in EEPROM
    - Serial protocol to ESP : DATA pH:... ppm:... v1:... v2:... v3:... v4:...
    - Setpoint Command:
      SETPH:...
      SETPPM:...
      SETONDUR:...
      SETOFFDUR:...
*/

#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

#define PH_PIN A2
#define PPM_PIN A3

#define VALVE1_PIN 8    // pH up
#define VALVE2_PIN 9    // pH down
#define VALVE3_PIN 10   // fertilizer A & B
#define VALVE4_PIN 11   // filter (also via Blynk controlled through ESP)

LiquidCrystal_I2C lcd(0x27, 20, 4);   // adjust address if needed

// Keypad setup
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '.', '0', '#', 'D' }
};
byte rowPins[ROWS] = { 2, 3, 4, 5 };
byte colPins[COLS] = { 6, 7, A0, A1 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// EEPROM addresses
const int ADDR_PHP = 0;       // float (4 bytes) -> PH Setpoint
const int ADDR_PPM = 10;      // int (2 bytes) -> PPM Setpoint
const int ADDR_ONDUR = 20;    // int (2 bytes) -> ON Duration Setpoint (seconds)
const int ADDR_OFFDUR = 24;   // int (2 bytes) -> OFF Duration Setpoint (minutes)

// STARTUP/INTRO config
const unsigned long INTRO_DURATION_MS = 3000UL; // Default 3 second
const unsigned long INTRO_ANIM_INTERVAL = 350UL; // Animation Interval (ms)
bool introSkipped = false;

// Sensor/read timing
unsigned long lastSensorSend = 0;
const unsigned long SENSOR_SEND_INTERVAL = 5000;  // ms -> send to ESP every 5s

// Control timers
unsigned long lastPHAction = 0;
bool phActionPending = false;
unsigned long lastPPMAction = 0;
bool ppmActionPending = false;

// Default durations
int defaultOnSec = 15;    // Default ON duration in seconds
int defaultOffMin = 2;    // Default OFF duration in minutes

// Runtime durations (editable)
int onDurationSec = 15;  // seconds valve ON
int offDurationMin = 2;  // minutes between adjustments (dead/off time)

// Derived ms values
unsigned long valveOnDurationMs = 15000UL;
unsigned long adjustIntervalMs = 120000UL;

// Setpoints sensor
float setPH = 7.0;
int setPPM = 500;

// Deadband sensor
float phUpDeadband = 0.8;
float phDownDeadband = 0.8;
float ppmDeadband = 5;

// Current readings
float currentPH = 7.0;
int currentPPM = 0;

// For scrolling IP in LCD
int ipScrollIndex = 0;
const int ipWindowSize = 12;  // Maximum length of display area for IP in this row

// Network status (from ESP)
bool wifiOK = false;
bool blynkOK = false;
String espIP = "0.0.0.0";

// ------ AP connect notification  ------
String apSSID = "";
String apIP = "";
bool apAlertActive = false;                         // Currently showing notification
unsigned long apAlertStartMs = 0;
unsigned long apLastTriggerMs = 0;
const unsigned long AP_ALERT_SHOW_MS = 10000UL;     // Show 10s
const unsigned long AP_ALERT_REPEAT_MS = 20000UL;   // Repeat every 20s if still not connected
unsigned long apBlinkLastMs = 0;
bool apBlinkState = false;
const unsigned long AP_BLINK_INTERVAL = 500UL;      // Backlight blink speed
bool savedBacklightState = true;                    // to restore after alert

// Last NET/DATA from ESP monitoring
unsigned long lastComRecvMs = 0;
const unsigned long COM_TIMEOUT_MS = 20000UL;  // 20s timeout

// For non-blocking valve activation
struct ValveState {
  bool active;
  unsigned long startMillis;
} valve1, valve2, valve3, valve4;

// Return remaining seconds for valve, or 0 if not active
unsigned long remainingSeconds(ValveState* v) {
  if (!v->active) return 0;
  long diffMs = (long)(v->startMillis + valveOnDurationMs - millis());
  if (diffMs <= 0) return 0;
  return (unsigned long)(diffMs / 1000UL);
}

// ------ Valve setup ------
void setupValves() {
  pinMode(VALVE1_PIN, OUTPUT);
  pinMode(VALVE2_PIN, OUTPUT);
  pinMode(VALVE3_PIN, OUTPUT);
  pinMode(VALVE4_PIN, OUTPUT);
  // Relay active LOW => set LOW to keep OFF (matching activate code below)
  digitalWrite(VALVE1_PIN, HIGH);   // high for active low
  digitalWrite(VALVE2_PIN, HIGH);
  digitalWrite(VALVE3_PIN, HIGH);
  digitalWrite(VALVE4_PIN, HIGH);
  valve1 = { false, 0 };            // false for active low
  valve2 = { false, 0 };
  valve3 = { false, 0 };
  valve4 = { false, 0 };
}

// ------ ON & OFF Duration Setpoint Calculation ------
void recalcDurations() {
  valveOnDurationMs = (unsigned long)onDurationSec * 1000UL;
  adjustIntervalMs = (unsigned long)offDurationMin * 60000UL;
  // safety min values
  if (valveOnDurationMs < 500UL) valveOnDurationMs = 500UL;
  if (adjustIntervalMs < 1000UL) adjustIntervalMs = 1000UL;
}

// ------ Startup Intro Screen ----------
void showStartupIntro(unsigned long duration_ms = INTRO_DURATION_MS) {
  unsigned long start = millis();
  unsigned long lastAnim = 0;
  int animStep = 0;
  introSkipped = false;

  // Ensure LCD on
  lcd.clear();
  lcd.backlight();

  // Static lines
  const String line0 = "  Fertilizer Mixer  ";    // 20 chars ideal
  const String line1 = "      MIX V1.0      ";
  const String line2 = "  by:Dit's Project  ";
  // Line3 will hold animation / hint
  lcd.setCursor(0,0); lcd.print(line0);
  lcd.setCursor(0,1); lcd.print(line1);
  lcd.setCursor(0,2); lcd.print(line2);
  lcd.setCursor(0,3); lcd.print(" Press any key");

  // Animation loop until time elapsed or key pressed
  while ((millis() - start) < duration_ms) {
    // Check keypad for skip (non-blocking)
    char k = keypad.getKey();
    if (k) { introSkipped = true; break; }

    // Simple dot animation on last line
    if (millis() - lastAnim >= INTRO_ANIM_INTERVAL) {
      lastAnim = millis();
      animStep = (animStep + 1) % 5;              // 0..4 dots
      lcd.setCursor(15,3);                        // Near right side
      for (int i=0;i<animStep;i++) lcd.print('.');
      for (int i=animStep;i<4;i++) lcd.print(' ');
    }
    // Small yield to avoid tight busy-loop
    delay(20);
  }
  // Clear intro and restore main display
  lcd.clear();
  // Optionally restore backlight or keep on
  lcd.backlight();
}

void setup() {
  Serial.begin(9600);   // Serial hardware to ESP (and USB) — disconnect ESP when uploading
  lcd.init();
  lcd.backlight();
  setupValves();

  // Read saved sens setpoints from EEPROM
  EEPROM.get(ADDR_PHP, setPH);
  if (isnan(setPH) || setPH < 0 || setPH > 14) setPH = 7.0;
  int tmpPPM;
  EEPROM.get(ADDR_PPM, tmpPPM);
  if (tmpPPM < 0 || tmpPPM > 100000) setPPM = 500;
  else setPPM = tmpPPM;

  // Read durations from EEPROM
  int tmpOn = 0;
  int tmpOff = 0;
  EEPROM.get(ADDR_ONDUR, tmpOn);
  EEPROM.get(ADDR_OFFDUR, tmpOff);
  if (tmpOn < 1 || tmpOn > 3600) onDurationSec = defaultOnSec;
  else onDurationSec = tmpOn;
  if (tmpOff < 0 || tmpOff > 10000) offDurationMin = defaultOffMin;
  else offDurationMin = tmpOff;
  recalcDurations();

  // Initial LCD
  showStartupIntro();
  updateLCD();
}

void loop() {
  handleKeypad();
  readSensors();
  handleControlLoops();
  handleSerialFromESP();
  //sendSensorToESPIfNeeded();
  updateLCDPeriodic();
  updateAPAlert();
  checkComTimeout();
  handleValveTimeouts();
}

// ------ Keypad handling + screen pages ------
String inputBuffer = "";
// for Sensor
bool enteringPH = false;
bool enteringPPM = false;
// for durations
bool enteringOnDur = false;
bool enteringOffDur = false;

unsigned long lcdRefresh = 0;
int screenPage = 1;   // 1 = main, 2 = durations, 3 = WiFi & Cloud Status

// Blink variables
bool blinkState = true;
unsigned long lastBlinkMillis = 0;
const unsigned long BLINK_INTERVAL = 750UL;  // 750 ms Blink interval

// Input timeout
unsigned long lastInputActivity = 0;
const unsigned long inputTimeout = 10000;  // 10 Seconds input timeout duration

void enterInputModeStart() {
  lastInputActivity = millis();
  lastBlinkMillis = millis();  // blink reset
  blinkState = true;
}

// Helper to count digits in a string
int countDigits(const String& s) {
  int c = 0;
  for (unsigned int i = 0; i <= s.length(); i++) {
    if (s[i] >= '0' && s[i] <= '9') c++;
  }
  return c;
}

// Helper to count digits after dot
int digitsAfterDot(const String& s) {
  int idx = s.indexOf('.');
  if (idx == -1) return 0;
  return s.length() - idx - 1;
}

// ------ Keypad input handling -----
void handleKeypad() {
  char k = keypad.getKey();
  if (!k) return;

  // Updhate last activity when a key is pressed (use for timeout)
  lastInputActivity = millis();

  // Global backspace
  if (k == 'C') {
    if (inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length() - 1);
    return;
  }

  // If currently entering pH or PPM (page 1)
  if (enteringPH || enteringPPM) {
    if (k == 'D') { // use D to cancel entry (no save)
      enteringPH = enteringPPM = false;
      inputBuffer = "";
      updateLCD();
      return;
    } else if (k == 'A' && enteringPH) {    // Confirm ph setpoint
      float val = inputBuffer.toFloat();
      if (val > 0.0 && val <= 14.0) {       // 1.0 .. 14.0 ph acceptable range
        setPH = val;
        EEPROM.put(ADDR_PHP, setPH);
        Serial.print("SETPH:");
        Serial.print(setPH);
        Serial.println();
      }
      enteringPH = false;
      inputBuffer = "";
      updateLCD();
      return;
    } else if (k == 'B' && enteringPPM) {   // Confirm ppm setpoint
      int val = inputBuffer.toInt();
      if (val > 0 && val <= 2000) {         // 1 .. 2000 ppm acceptable range
        setPPM = val;
        EEPROM.put(ADDR_PPM, setPPM);
        Serial.print("SETPPM:");
        Serial.print(setPPM);
        Serial.println();
      }
      enteringPPM = false;
      inputBuffer = "";
      updateLCD();
      return;
    } else {
      // Numeric input
      if (enteringPH) {
        if ((k >= '0' && k <= '9')) {
          int currDigitCount = countDigits(inputBuffer);
          if (currDigitCount < 4) {
            // If there's a dot and already 2 digits after dot, reject
            if (inputBuffer.indexOf('.') != -1) {
              if (digitsAfterDot(inputBuffer) < 2) {
                inputBuffer += k;
              } else {
                // ignore (max 2 decimals)
              }
            } else {
              inputBuffer += k;
            }
          }
        } else if (k == '.') {
          // Allow dot only of not already present
          if (inputBuffer.indexOf('.') == -1) {
            // Optionally prevent '.' as first char by prefixing 0
            if (inputBuffer.length() == 0) {
              inputBuffer += '0';
            }
            inputBuffer += '.';
          }
        }
      } else if (enteringPPM) {
        if ((k >= '0' && k <= '9') && inputBuffer.length() < 4) inputBuffer += k;
      }
      return;
    }
  }

  // If currently entering durations (page 2)
  if (enteringOnDur || enteringOffDur) {
    if (k == 'D') {  // use D to cancel entry (no save)
      enteringOnDur = enteringOffDur = false;
      inputBuffer = "";
      updateLCD();
      return;
    } else if (k == 'A' && enteringOnDur) {   // Confirm on duration
      int val = inputBuffer.toInt();
      if (val > 0 && val <= 3600) {           // 1s .. 3600s second acceptable range
        onDurationSec = val;
        EEPROM.put(ADDR_ONDUR, onDurationSec);
        recalcDurations();
        Serial.print("SETONDUR:");
        Serial.println(onDurationSec);
      }
      enteringOnDur = false;
      inputBuffer = "";
      updateLCD();
      return;
    } else if (k == 'B' && enteringOffDur) {    // confirm off duration
      int val = inputBuffer.toInt();
      if (val > 0 && val <= 10000) {            // 1min .. 10000min minutes acceptable range
        offDurationMin = val;
        EEPROM.put(ADDR_OFFDUR, offDurationMin);
        recalcDurations();
        Serial.print("SETOFFDUR:");
        Serial.println(offDurationMin);
      }
      enteringOffDur = false;
      inputBuffer = "";
      updateLCD();
      return;
    } else {
      // Numeric entry
      if ((k >= '0' && k <= '9')) {
        inputBuffer += k;
      }
      return;
    }
  }

  // Not entering anything -> normal key actions depend on screen page
  if (k == 'A') {
    if (!apAlertActive && screenPage != 3) {
      if (screenPage == 1) {
        // page 1 -> start editing ph setpoint
        enteringPH = true;
        enteringPPM = false;
        inputBuffer = "";
        enterInputModeStart();
      } else if (screenPage == 2) {
        // page 2 -> start editing ON duration
        enteringOnDur = true;
        enteringOffDur = false;
        inputBuffer = "";
        enterInputModeStart();
      } else {
      }
      updateLCD();
      return;
    }
  } else if (k == 'B') {
    if (!apAlertActive && screenPage != 3) {
      if (screenPage == 1) {
        // page 1 -> start editing ppm setpoint
        enteringPPM = true;
        enteringPH = false;
        inputBuffer = "";
        enterInputModeStart();
      } else if (screenPage == 2) {
        // page 2 -> start editing OFF duration
        enteringOffDur = true;
        enteringOnDur = false;
        inputBuffer = "";
        enterInputModeStart();
      } else {
      }
      updateLCD();
      return;
    }
  } else if (k == 'D') {
    // Toggle / scroll pages only when NOT entering input mode & active alert
    if (!apAlertActive) {
      if (!(enteringPH || enteringPPM || enteringOnDur || enteringOffDur)) {
        screenPage++;                        // next page
        if (screenPage > 3) screenPage = 1;  // wrap-around (1..3)
        updateLCD();
      } else {
        // If currently entering input, D acts as "cancel" (keep existing behavior)
        enteringPH = enteringPPM = enteringOnDur = enteringOffDur = false;
        inputBuffer = "";
        updateLCD();
      }
      return;
    }
  }
}

// ------ Sensor reading ------
unsigned long lastAnalogRead = 0;
const unsigned long ANALOG_INTERVAL = 2000;  // Every 2s read sensors for display
void readSensors() {
  unsigned long now = millis();
  if (now - lastAnalogRead < ANALOG_INTERVAL) return;
  lastAnalogRead = now;
  // Average multiple ADC samples for stability
  const int SAMPLES = 10;
  long sumPH = 0, sumPPM = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sumPH += analogRead(PH_PIN);
    sumPPM += analogRead(PPM_PIN);
    delay(5);
  }
  float adcPH = sumPH / (float)SAMPLES;
  float adcPPM = sumPPM / (float)SAMPLES;
  // Apply formula
  currentPH = (-0.0267857 * adcPH) + 21.6964;
  currentPPM = (int)((1.0797 * (adcPPM * 2000.0) / 1023.0) - 3.1);
}

// ---------- Control logic ----------
void handleControlLoops() {
  unsigned long now = millis();
  // pH control: only if not currently actuating pH valves (1 or 2)
  if (!valve1.active && !valve2.active) {
    if (currentPH < setPH - phUpDeadband) {
      if (!phActionPending || (now - lastPHAction >= adjustIntervalMs)) {
        activateValve(&valve1, VALVE1_PIN);
        lastPHAction = now;
        phActionPending = true;
      }
    } else if (currentPH > setPH + phDownDeadband) {
      if (!phActionPending || (now - lastPHAction >= adjustIntervalMs)) {
        activateValve(&valve2, VALVE2_PIN);
        lastPHAction = now;
        phActionPending = true;
      }
    }
  }
  // ppm control
  if (!valve3.active) {
    if (currentPPM < setPPM - ppmDeadband) {
      if (!ppmActionPending || (now - lastPPMAction >= adjustIntervalMs)) {
        activateValve(&valve3, VALVE3_PIN);
        lastPPMAction = now;
        ppmActionPending = true;
      }
    }
  }
}

// Helper for Valve
void activateValve(ValveState* v, uint8_t pin) {
  digitalWrite(pin, LOW);     // Active LOW turns relay ON
  v->active = true;
  v->startMillis = millis();
  // Record action times
  if (pin == VALVE1_PIN || pin == VALVE2_PIN) {
    lastPHAction = millis();
    phActionPending = true;
  } else if (pin == VALVE3_PIN) {
    lastPPMAction = millis();
    ppmActionPending = true;
  } else if (pin == VALVE4_PIN) {
    // Nothing
  }
}

// Valve timeout handling
void handleValveTimeouts() {
  unsigned long now = millis();
  ValveState* valves[3] = { &valve1, &valve2, &valve3 };
  uint8_t pins[3] = { VALVE1_PIN, VALVE2_PIN, VALVE3_PIN };
  for (int i = 0; i < 3; i++) {
    if (valves[i]->active) {
      if (now - valves[i]->startMillis >= valveOnDurationMs) {
        digitalWrite(pins[i], HIGH);  // turn OFF
        valves[i]->active = false;
        // leave lastAction timestamp (for interval checking)
      }
    }
  }
}

// Valve Status for lcd display
void valveStatus() {
  // Valve statuses + remaining (rows 2 & 3)
  // line 2: V1 & V2
  lcd.setCursor(0, 2);
  // V1
  lcd.print("V1:");
  if (valve1.active) {
    lcd.print("ON ");
    unsigned long r1 = remainingSeconds(&valve1);
    // pad to 3 chars
    if (r1 < 10) lcd.print("  ");
    else if (r1 < 100) lcd.print(" ");
    lcd.print(r1);
    lcd.print("s");
  } else {
    lcd.print("OFF    ");
  }
  // spacing then V2
  lcd.setCursor(10, 2);
  lcd.print("V2:");
  if (valve2.active) {
    lcd.print("ON ");
    unsigned long r2 = remainingSeconds(&valve2);
    if (r2 < 10) lcd.print("  ");
    else if (r2 < 100) lcd.print(" ");
    lcd.print(r2);
    lcd.print("s");
  } else {
    lcd.print("OFF    ");
  }

  // line 3: V3 & V4
  lcd.setCursor(0, 3);
  lcd.print("V3:");
  if (valve3.active) {
    lcd.print("ON ");
    unsigned long r3 = remainingSeconds(&valve3);
    if (r3 < 10) lcd.print("  ");
    else if (r3 < 100) lcd.print(" ");
    lcd.print(r3);
    lcd.print("s");
  } else {
    lcd.print("OFF    ");
  }

  lcd.setCursor(10, 3);
  lcd.print("V4:");
  if (valve4.active) {
    lcd.print("ON ");
  } else {
    lcd.print("OFF    ");
  }
}

// ------- AP Alert ------
// Center text helper for 20-char LCD
void lcdPrintCentered(const String& s, uint8_t row) {
  int len = s.length();
  int start = 0;
  if (len < 20) start = (20 - len) / 2;
  lcd.setCursor(start, row);
  lcd.print(s);
  // pad remainder to avoid leftover chars
  for (int i = start + len; i < 20; ++i) lcd.print(' ');
}

// Start AP notification (call when IP seen)
void startAPAlert() {
  apAlertActive = true;
  apAlertStartMs = millis();
  // only set last-trigger timestamp if it wasn't set (avoid immediate retrigger)
  if (apLastTriggerMs == 0) apLastTriggerMs = millis();
  apBlinkLastMs = millis();
  apBlinkState = true;
  savedBacklightState = true;  // assume backlight on
  lcd.backlight();             // ensure on at start
  // draw immediately
  lcd.clear();

  lcdPrintCentered("Connect to Wifi", 0);
  lcdPrintCentered(apSSID, 1);
  lcdPrintCentered("IP: " + apIP, 2);
  lcd.setCursor(0, 3);
  lcd.print("Waiting...        ");
}

// Stop AP notification (call when NET says WIFI connected)
void stopAPAlert() {
  if (!apAlertActive) {
    // But still ensure we clear scheduled triggers so it won't reappear
    apLastTriggerMs = 0;
    apSSID = "";
    apIP = "";
    return;
  }
  apAlertActive = false;
  // Restore backlight (use savedBacklightState if tracked; otherwise enable)
  lcd.backlight();
  // Reset trigger so it won't auto-restart
  apLastTriggerMs = 0;
  apSSID = "";
  apIP = "";
  // Redraw current page (no flicker if caller is NET parser only)
  updateLCD();
}

// Update AP alert state
void updateAPAlert() {
  unsigned long now = millis();
  // If WiFi already connected, ensure alert is completely stopped
  if (wifiOK) {
    if (apAlertActive) {
      apAlertActive = false;
      lcd.backlight();
      apLastTriggerMs = 0;
      apSSID = "";
      apIP = "";
      updateLCD();
    }
    return;
  }

  // If not active, check whether to retrigger (only if we have stored SSID/IP and a last-trigger time)
  if (!apAlertActive) {
    if (apSSID.length() > 0 && apIP.length() > 0 && apLastTriggerMs > 0) {
      if (now - apLastTriggerMs >= AP_ALERT_REPEAT_MS) {
        // Retrigger
        startAPAlert();
        // Cancel input mode
        enteringPPM = false;
        enteringPH = false;
        enteringOffDur = false;
        enteringOnDur = false;
      }
    }
    return;
  }

  // Handle blinking backlight
  if (now - apBlinkLastMs >= AP_BLINK_INTERVAL) {
    apBlinkLastMs = now;
    apBlinkState = !apBlinkState;
    if (apBlinkState) lcd.backlight();
    else lcd.noBacklight();
  }

  // Auto-hide after AP_ALERT_SHOW_MS
  if (now - apAlertStartMs >= AP_ALERT_SHOW_MS) {
    // Hide alert but keep apLastTriggerMs so it can retrigger later
    apAlertActive = false;
    lcd.backlight();        // ensure backlight on after hiding
    apLastTriggerMs = now;  // start repeat timer
    updateLCD();            // redraw current page
  }
}

// ------ Atmega328P & ESP8266 communiation timeout ------
// Call this periodically
void checkComTimeout() {
  unsigned long now = millis();
  // If you have never received any, ignore it (or set the default status)
  if (lastComRecvMs == 0) return;
  // Reset network status after COM_TIMEOUT
  if (now - lastComRecvMs > COM_TIMEOUT_MS) {
    // Only perform reset if something is currently "true" (avoid useless redraw)
    if (wifiOK || blynkOK || espIP != "0.0.0.0") {
      wifiOK = false;
      blynkOK = false;
      espIP = "0.0.0.0";
      // Update LCD if page is showing network or to refresh page3
      if (screenPage == 3) updateLCD();  // Refresh immediately if page3 visible
    }
    // To avoid repeatedly doing the same reset, set lastComRecvMs to now (or 0)
    lastComRecvMs = now;  // Next timeout will wait another COM_TIMEOUT_MS
  }
}

// ------ Serial handling (incoming commands from ESP) ------
String rxLine = "";
void handleSerialFromESP() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lastComRecvMs = millis();  // Mark that just received something from ESP
      processSerialCommand(rxLine);
      rxLine = "";
    } else if (c != '\r') {
      rxLine += c;
    }
  }
}

void processSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (cmd.startsWith("SETPH:")) {
    String v = cmd.substring(6);
    float val = v.toFloat();
    if (val >= 0.0 && val <= 14.0) {
      setPH = val;
      EEPROM.put(ADDR_PHP, setPH);
      updateLCD();
    }
  } else if (cmd.startsWith("SETPPM:")) {
    String v = cmd.substring(7);
    int val = v.toInt();
    if (val >= 0 && val <= 2000) {
      setPPM = val;
      EEPROM.put(ADDR_PPM, setPPM);
      updateLCD();
    }
  } else if (cmd.startsWith("VALVE4 ON")) {   //  Valve 4 ON state set from ESP
    digitalWrite(VALVE4_PIN, LOW);
    valve4.active = true;
    valve4.startMillis = millis();
  } else if (cmd.startsWith("VALVE4 OFF")) {  //  Valve 4 OFF state set from ESP
    digitalWrite(VALVE4_PIN, HIGH);
    valve4.active = false;
  } else if (cmd.startsWith("DATREQ")) {
    // Send immediate reading out
    sendSensorNow();
  } else if (cmd == "SETREQ") {
    // Send setpoint when requested by ESP
    sendSetpointsToESP();
  } else if (cmd.startsWith("NET")) {     // Network Status from ESP
    int idx;
    String s;
    idx = cmd.indexOf("WIFI:");
    if (idx >= 0) {
      int idx2 = cmd.indexOf(' ', idx);
      s = (idx2 > idx) ? cmd.substring(idx + 5, idx2) : cmd.substring(idx + 5);
      wifiOK = (s.toInt() == 1);
      if (wifiOK) {
        // connected -> stop AP alert if any
        stopAPAlert();
      }
    }
    idx = cmd.indexOf("BLYNK:");
    if (idx >= 0) {
      int idx2 = cmd.indexOf(' ', idx);
      s = (idx2 > idx) ? cmd.substring(idx + 6, idx2) : cmd.substring(idx + 6);
      blynkOK = (s.toInt() == 1);
    }
    idx = cmd.indexOf("IP:");
    if (idx >= 0) {
      int idx2 = cmd.indexOf(' ', idx);
      espIP = (idx2 > idx) ? cmd.substring(idx + 3, idx2) : cmd.substring(idx + 3);
    }
    // Only refresh the LCD when on page 3 (avoid flashing)
    if (screenPage == 3) updateLCD();
    return;

  } else if (cmd.startsWith("*wm:")) {      // Handle wifi Manager message for alert 
    int pos1 = cmd.indexOf("StartAP with SSID:");
    if (pos1 >= 0) {
      // ambil substring setelah teks tersebut
      int start = pos1 + strlen("StartAP with SSID:");
      apSSID = cmd.substring(start);
      apSSID.trim();
    }
    int pos2 = cmd.indexOf("AP IP address:");
    if (pos2 >= 0) {
      int start = pos2 + strlen("AP IP address:");
      apIP = cmd.substring(start);
      apIP.trim();
      // Trigger alert
      startAPAlert();
    }
  } else if (cmd.startsWith("SETONDUR:")) {       // ON Duration set from ESP
    String v = cmd.substring(9);
    int val = v.toInt();
    if (val >= 1 && val <= 3600) {
      onDurationSec = val;
      EEPROM.put(ADDR_ONDUR, onDurationSec);
      recalcDurations();
      updateLCD();
    }
  } else if (cmd.startsWith("SETOFFDUR:")) {    // OFF Duration set from ESP
    String v = cmd.substring(10);
    int val = v.toInt();
    if (val >= 0 && val <= 10000) {
      offDurationMin = val;
      EEPROM.put(ADDR_OFFDUR, offDurationMin);
      recalcDurations();
      updateLCD();
    }
  }
}

// ------ Sending sensors & valve status to ESP ------
void sendSensorNow() {
  Serial.print("DATA pH:");
  Serial.print(currentPH, 2);
  Serial.print(" ppm:");
  Serial.print(currentPPM);
  Serial.print(" v1:");
  Serial.print(valve1.active ? 1 : 0);
  Serial.print(" v2:");
  Serial.print(valve2.active ? 1 : 0);
  Serial.print(" v3:");
  Serial.print(valve3.active ? 1 : 0);
  Serial.print(" v4:");
  Serial.print(valve4.active ? 1 : 0);
  Serial.println();
}

// Helper: send setpoint/duration when requested
void sendSetpointsToESP() {
  Serial.print("SETPH:");
  Serial.println(setPH, 2);
  Serial.print("SETPPM:");
  Serial.println(setPPM);
  Serial.print("SETONDUR:");
  Serial.println(onDurationSec);
  Serial.print("SETOFFDUR:");
  Serial.println(offDurationMin);
}

void sendSensorToESPIfNeeded() {
  unsigned long now = millis();
  if (now - lastSensorSend >= SENSOR_SEND_INTERVAL) {
    lastSensorSend = now;
    sendSensorNow();
  }
}

// ---------- LCD ----------
void updateLCD() {
  lcd.clear();
  if (screenPage == 1) {    // Page 1 PH & PPM
    lcd.setCursor(0, 0);
    lcd.print("IN PH  = ");
    lcd.print(setPH, 2);
    lcd.setCursor(0, 1);
    lcd.print("IN PPM = ");
    lcd.print(setPPM);
    lcd.setCursor(0, 2);
    lcd.print("PH     = ");
    lcd.print(currentPH, 2);
    lcd.setCursor(0, 3);
    lcd.print("PPM    = ");
    lcd.print(currentPPM);
  } else if (screenPage == 2) { // Page 2 Duration
    lcd.setCursor(0, 0);
    lcd.print("ON DUR  = ");
    lcd.print(onDurationSec);
    lcd.print(" sec");
    lcd.setCursor(0, 1);
    lcd.print("OFF DUR = ");
    lcd.print(offDurationMin);
    lcd.print(" min");
    valveStatus();
  } else {                      // Page 3 Network status
    lcd.setCursor(0, 0);
    lcd.print("WIFI  : ");
    lcd.print(wifiOK ? "Connected   " : "DISCONNECTED");
    lcd.setCursor(0, 1);
    lcd.print("BLYNK : ");
    lcd.print(blynkOK ? "Connected" : "OFFLINE  ");
    lcd.setCursor(0, 2);
    lcd.print("IP    : ");
    // If IP length is small → display directly
    if (espIP.length() <= ipWindowSize) {
      lcd.print(espIP);
      // Add padding
      int pad = ipWindowSize - espIP.length();
      while (pad--) lcd.print(' ');
    }
    lcd.setCursor(0, 3);
    lcd.print("MODE  : ");
    if (blynkOK) lcd.print("CLOUD OK  ");
    else if (wifiOK) lcd.print("NO CLOUD  ");
    else lcd.print("LOCAL ONLY");
  }
}

// ------------ LCD Update -------------
unsigned long lastLCDUpdate = 0;
void updateLCDPeriodic() {
  unsigned long now = millis();

  // handle blink timing
  if (now - lastBlinkMillis >= BLINK_INTERVAL) {
    lastBlinkMillis = now;
    blinkState = !blinkState;
  }

  // check input timeout: if in any entering mode and no activity for INPUT_TIMEOUT_MS, cancel entry
  if ((enteringPH || enteringPPM || enteringOnDur || enteringOffDur) && (now - lastInputActivity >= inputTimeout)) {
    // cancel input
    enteringPH = enteringPPM = enteringOnDur = enteringOffDur = false;
    inputBuffer = "";
    updateLCD();
  }

  if (now - lastLCDUpdate > 500) {      // Every 500ms LCD Update
    lastLCDUpdate = now;
    if ((screenPage == 1) && !apAlertActive) {
      if (enteringPH) {
        lcd.setCursor(9, 0);
        if (blinkState) {
          lcd.print("     ");
          lcd.setCursor(9, 0);
          lcd.print(inputBuffer);
        } else {
          lcd.print("     ");
        }
      } else if (enteringPPM) {
        lcd.setCursor(9, 1);
        if (blinkState) {
          lcd.print("    ");
          lcd.setCursor(9, 1);
          lcd.print(inputBuffer);
        } else {
          lcd.print("    ");
        }
      }
      lcd.setCursor(9, 2);
      lcd.print("       ");
      lcd.setCursor(9, 2);
      lcd.print(currentPH, 2);
      lcd.setCursor(9, 3);
      lcd.print("       ");
      lcd.setCursor(9, 3);
      lcd.print(currentPPM);
    } else if ((screenPage == 2) && !apAlertActive) {
      if (enteringOnDur) {
        lcd.setCursor(10, 0);
        if (blinkState) {
          lcd.print("        ");
          lcd.setCursor(10, 0);
          lcd.print(inputBuffer);
        } else {
          lcd.print("        ");
        }
        valveStatus();
      } else if (enteringOffDur) {
        lcd.setCursor(10, 1);
        if (blinkState) {
          lcd.print("         ");
          lcd.setCursor(10, 1);
          lcd.print(inputBuffer);
        } else {
          lcd.print("         ");
        }
        valveStatus();
      } else {
        valveStatus();
      }
    } else {
      // If IP is longer than ipWindowSize -> IP Scrolling
      lcd.setCursor(8, 2);
      if (espIP.length() > ipWindowSize) {
        ipScrollIndex++;
        if (ipScrollIndex > espIP.length()) ipScrollIndex = 0;
        // take substring according to window
        String window = espIP + "   ";  // add spaces for smooth scrolling
        int idx = ipScrollIndex;
        if (idx + ipWindowSize > window.length()) idx = 0;
        lcd.print(window.substring(idx, idx + ipWindowSize));
      }
    }
  }
}