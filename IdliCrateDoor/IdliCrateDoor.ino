/* ==================================================================================
  Idli's Crate Door  -  v2.0.3

  Motorized dog-crate door on an ESP32-C6 SuperMini.
    - Continuous-rotation MG996R servo drives the door through a worm gear and a
      torque-limiting clutch.
    - Limit switches at the open and closed positions.
    - Momentary pushbutton on top of the opener.

  ESP RainMaker creates three devices:
    1. "Idlis Door" (Exterior Blinds)       - Alexa "open" / "close".
    2. "Idlis Food Button" (Contact Sensor) - reports "open" for 2 s after a long press,
                                              to trigger an Alexa routine (e.g. a feeder).
    3. "Idli Door Settings" (custom type)   - settings, status and activity in the
                                              RainMaker app. Not discovered by Alexa.

  Button (acted on when released)
    Short press  0.1 - 2 s   toggle the door
    Long press   2 - 10 s    close the door and trigger the Food Button
    Held > 10 s              treated as stuck: ignored until released, alert sent

  Door hold
    If the closed door is pushed off its closed switch (the clutch can slip), the door
    is re-closed: 3 s attempt, then retries after 10 / 30 / 60 s. If all fail, the motor
    stays off, the door is marked faulted and a RainMaker alert is sent.

  Jam handling
    Each move times out at 1.5x its learned travel time (7 s until learned). A timeout
    stops the motor, marks a fault and sends an alert. A new command clears the fault,
    and so does the door arriving back at the requested switch on its own.

  ESP32 Arduino core 3.3.7 (Espressif)
  ESP32Servo 3.1.3 (Kevin Harrington)
  Partition scheme: RainMaker 4MB (needs the OTA slots)
=================================================================================== */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiProv.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <RMaker.h>
#include <AppInsights.h>
#include <esp_ota_ops.h>
#include <time.h>

#define FW_VERSION "2.0.3"

// ---- Provisioning (only used until the board joins Wi-Fi) -----------------------
// The "PROV_" prefix is what the RainMaker app looks for when scanning.
// No proof-of-possession: the app connects without asking for one. The Bluetooth
// link is still encrypted. To require one, set pop to a string, e.g. "idli1234".
const char *service_name = "PROV_IdliDoor";
const char *pop = nullptr;

// ---- Pins -----------------------------------------------------------------------
constexpr uint8_t PIN_SERVO = 1;
constexpr uint8_t PIN_LED = 8;  // onboard RGB LED (hidden inside the opener, kept off)
constexpr uint8_t PIN_LIM_OPEN = 15;
constexpr uint8_t PIN_LIM_CLOSE = 18;
constexpr uint8_t PIN_BUTTON = 19;

// ---- Servo (continuous rotation: value sets speed and direction) -----------------
constexpr int SERVO_OPEN = 180;
constexpr int SERVO_CLOSE = 0;
constexpr int SERVO_STOP = 90;

// ---- Timing ---------------------------------------------------------------------
constexpr uint32_t SWITCH_DEBOUNCE_MS = 20;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t SHORT_PRESS_MIN_MS = 100;
constexpr uint32_t LONG_PRESS_MS = 2000;
constexpr uint32_t STUCK_BUTTON_MS = 10000;
constexpr uint32_t FEED_PULSE_MS = 2000;  // how long the Food Button reads "open"

constexpr uint32_t MOVE_TIMEOUT_DEFAULT_MS = 7000;  // used until travel time is learned
constexpr float MOVE_TIMEOUT_FACTOR = 1.5f;
constexpr uint32_t MOVE_TIMEOUT_MIN_MS = 2000;
constexpr uint32_t MOVE_TIMEOUT_MAX_MS = 10000;

constexpr uint32_t HOLD_CONFIRM_MS = 300;   // door must stay off the switch this long
constexpr uint32_t HOLD_ATTEMPT_MS = 3000;  // each re-close attempt
constexpr uint8_t HOLD_RETRIES = 3;         // retries after the first attempt
constexpr uint32_t HOLD_BACKOFF_MS[HOLD_RETRIES] = { 10000, 30000, 60000 };

constexpr uint8_t LEARN_SAMPLES = 5;     // full moves averaged into the baseline
constexpr float SLOW_FACTOR = 1.3f;      // alert when a move is 30% slower than baseline
constexpr uint32_t SLOW_ALERT_COOLDOWN_MS = 24UL * 60 * 60 * 1000;
constexpr uint32_t OTA_CONFIRM_AFTER_MS = 30000;

// ---- RainMaker names ------------------------------------------------------------
// "Door Postion" keeps its original spelling so Alexa doesn't need re-discovery.
const char *DOOR_DEVICE = "Idlis Door";
const char *DOOR_PARAM = "Door Postion";
const char *FEED_DEVICE = "Idlis Food Button";
const char *FEED_PARAM = "Food Button State";
const char *SETTINGS_DEVICE = "Idli Door Settings";
const char *P_STATUS = "Door Status";
const char *P_POWERUP = "Power-up Behavior";
const char *P_LOCK = "Button Lock";
const char *P_OPENS = "Opens Today";
const char *P_FEEDS = "Feeds Today";
const char *P_LAST_OPENED = "Last Opened";
const char *P_OPEN_TIME = "Open Travel (s)";
const char *P_CLOSE_TIME = "Close Travel (s)";
const char *P_RESET_TRAVEL = "Reset Travel Times";
const char *P_FIRMWARE = "Firmware";

const char *POWERUP_MODES[] = { "Stay", "Close", "Last state" };
enum PowerUpMode : uint8_t { PU_STAY = 0, PU_CLOSE = 1, PU_LAST = 2 };

// ---- Types ----------------------------------------------------------------------
enum Target : int8_t { CLOSED = 0, OPEN = 1 };
enum MotionState : uint8_t { M_IDLE, M_MOVING };
enum HoldState : uint8_t { H_IDLE, H_CONFIRM, H_DRIVE, H_BACKOFF };

struct Input {
  uint8_t pin;
  uint32_t debounceMs;
  bool stable;
  bool lastRaw;
  uint32_t lastChange;
};

struct Travel {
  float baseline;  // seconds, 0 = still learning
  uint8_t n;
  float sum;
  bool slowAlerted;
  uint32_t lastSlowAlert;
};

// ---- Forward declarations -------------------------------------------------------
void sysProvEvent(arduino_event_t *sys_event);
void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx);
void requestDoor(int8_t target, const char *source);
void enterFault(const char *msg);

// On core 3.x, BLE provisioning can start in freed Bluetooth RAM unless this
// returns true. It sits below the type definitions on purpose: the Arduino IDE
// inserts its auto-generated prototypes just above the first function in the
// sketch, and they need Input and the other types to be declared already.
bool btInUse() {
  return true;
}

// ---- Globals --------------------------------------------------------------------
Servo servo;
Preferences prefs;

static Device *doorDev = nullptr;
static Device *feedDev = nullptr;
static Device *settingsDev = nullptr;

Input limOpen = { PIN_LIM_OPEN, SWITCH_DEBOUNCE_MS, HIGH, HIGH, 0 };
Input limClose = { PIN_LIM_CLOSE, SWITCH_DEBOUNCE_MS, HIGH, HIGH, 0 };
Input button = { PIN_BUTTON, BUTTON_DEBOUNCE_MS, HIGH, HIGH, 0 };

// Written by the RainMaker task, handled in loop()
volatile bool rmDoorPending = false;
volatile int8_t rmDoorTarget = CLOSED;
volatile bool rmPowerupPending = false;
volatile uint8_t rmPowerupMode = PU_STAY;
volatile bool rmLockPending = false;
volatile bool rmLockValue = false;
volatile bool rmResetTravelPending = false;
volatile int lastReportedPos = -1;

// Door
int8_t desired = CLOSED;
MotionState motion = M_IDLE;
uint32_t moveStart = 0;
uint32_t moveTimeout = MOVE_TIMEOUT_DEFAULT_MS;
bool moveFromLimit = false;  // move started at the opposite switch (full travel)
bool closedConfirmed = false;
int currentServo = -1;

// Door hold
HoldState hold = H_IDLE;
uint8_t holdAttempts = 0;
uint32_t holdTimer = 0;

// Fault
bool faultActive = false;
bool switchFault = false;
char faultText[64] = "";

// Settings
uint8_t powerupMode = PU_STAY;
bool buttonLocked = false;

// Button
bool btnDown = false;
bool btnStuck = false;   // stuck alert already sent for this press
bool btnIgnore = false;  // ignore this press when it's released
uint32_t btnPressStart = 0;
bool feedPulse = false;
uint32_t feedPulseStart = 0;

// Travel learning, indexed by Target
Travel travel[2] = {};
bool travelDirty = true;

// Activity counters
int opensToday = 0;
int feedsToday = 0;
int counterDay = -1;  // year * 1000 + day of year
char lastOpened[40] = "--";
bool countersDirty = true;

char lastStatus[80] = "";

// ===================================================================================
// Helpers
// ===================================================================================
void initInput(Input &in) {
  in.stable = in.lastRaw = digitalRead(in.pin);
  in.lastChange = millis();
}

void readInput(Input &in, uint32_t now) {
  bool raw = digitalRead(in.pin);
  if (raw != in.lastRaw) {
    in.lastRaw = raw;
    in.lastChange = now;
  } else if (raw != in.stable && now - in.lastChange >= in.debounceMs) {
    in.stable = raw;
  }
}

inline bool atOpen() {
  return limOpen.stable == LOW;
}
inline bool atClosed() {
  return limClose.stable == LOW;
}

void setMotor(int value) {
  if (value != currentServo) {
    servo.write(value);
    currentServo = value;
  }
}

void sendAlert(const char *msg) {
  Serial.printf("ALERT: %s\n", msg);
  esp_rmaker_raise_alert(msg);
}

bool localNow(struct tm &t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // not synced yet
  localtime_r(&now, &t);
  return true;
}

// ---- Persistence ----------------------------------------------------------------
void saveCounters() {
  prefs.putInt("opens", opensToday);
  prefs.putInt("feeds", feedsToday);
  prefs.putInt("day", counterDay);
  prefs.putString("lastOpen", lastOpened);
}

void saveTravel(int8_t t) {
  char k[4] = { 't', 'b', (char)('0' + t), 0 };
  prefs.putFloat(k, travel[t].baseline);
  k[1] = 'n';
  prefs.putUChar(k, travel[t].n);
  k[1] = 's';
  prefs.putFloat(k, travel[t].sum);
}

void loadTravel(int8_t t) {
  char k[4] = { 't', 'b', (char)('0' + t), 0 };
  travel[t].baseline = prefs.getFloat(k, 0);
  k[1] = 'n';
  travel[t].n = prefs.getUChar(k, 0);
  k[1] = 's';
  travel[t].sum = prefs.getFloat(k, 0);
}

// ===================================================================================
// Door control
// ===================================================================================
uint32_t moveTimeoutFor(int8_t t) {
  float b = travel[t].baseline;
  if (b <= 0) return MOVE_TIMEOUT_DEFAULT_MS;
  uint32_t ms = (uint32_t)(b * 1000.0f * MOVE_TIMEOUT_FACTOR);
  if (ms < MOVE_TIMEOUT_MIN_MS) ms = MOVE_TIMEOUT_MIN_MS;
  if (ms > MOVE_TIMEOUT_MAX_MS) ms = MOVE_TIMEOUT_MAX_MS;
  return ms;
}

// Every command (button, Alexa, app, power-up) comes through here.
void requestDoor(int8_t target, const char *source) {
  Serial.printf("Door -> %s (%s)\n", target == OPEN ? "OPEN" : "CLOSED", source);

  faultActive = false;
  switchFault = false;
  faultText[0] = 0;
  hold = H_IDLE;
  holdAttempts = 0;
  desired = target;
  if (prefs.getChar("lastDesired", -1) != target) prefs.putChar("lastDesired", target);

  bool alreadyThere = (target == OPEN) ? atOpen() : atClosed();
  if (alreadyThere) {
    setMotor(SERVO_STOP);
    motion = M_IDLE;
    closedConfirmed = (target == CLOSED);
    return;
  }

  closedConfirmed = false;
  moveFromLimit = (target == OPEN) ? atClosed() : atOpen();
  moveStart = millis();
  moveTimeout = moveTimeoutFor(target);
  motion = M_MOVING;
  setMotor(target == OPEN ? SERVO_OPEN : SERVO_CLOSE);
}

void enterFault(const char *msg) {
  setMotor(SERVO_STOP);
  motion = M_IDLE;
  hold = H_IDLE;
  holdAttempts = 0;
  closedConfirmed = false;
  faultActive = true;
  snprintf(faultText, sizeof(faultText), "%s", msg);

  char alertMsg[96];
  snprintf(alertMsg, sizeof(alertMsg), "Idli's door: %s", msg);
  sendAlert(alertMsg);
}

void noteOpened() {
  opensToday++;
  struct tm t;
  if (localNow(t)) strftime(lastOpened, sizeof(lastOpened), "%a %b %d, %I:%M %p", &t);
  saveCounters();
  countersDirty = true;
}

void learnTravel(int8_t t, uint32_t ms) {
  Travel &tr = travel[t];
  float s = ms / 1000.0f;

  if (tr.baseline <= 0) {  // still learning
    tr.sum += s;
    tr.n++;
    if (tr.n >= LEARN_SAMPLES) {
      tr.baseline = tr.sum / tr.n;
      Serial.printf("%s travel baseline learned: %.2f s\n", t == OPEN ? "Open" : "Close", tr.baseline);
      travelDirty = true;
    }
    saveTravel(t);
    return;
  }

  if (s > tr.baseline * SLOW_FACTOR) {
    uint32_t now = millis();
    if (!tr.slowAlerted || now - tr.lastSlowAlert >= SLOW_ALERT_COOLDOWN_MS) {
      char msg[120];
      snprintf(msg, sizeof(msg), "Idli's door took %.1f s to %s (normally %.1f s). Check the mechanism.",
               s, t == OPEN ? "open" : "close", tr.baseline);
      sendAlert(msg);
      tr.slowAlerted = true;
      tr.lastSlowAlert = now;
    }
  }
}

void resetTravel() {
  for (int8_t t = CLOSED; t <= OPEN; t++) {
    travel[t] = {};
    saveTravel(t);
  }
  travelDirty = true;
  Serial.println("Travel times reset - relearning over the next 5 full moves");
}

void updateMotion() {
  if (motion != M_MOVING) return;

  // Read the clock here rather than using the loop's "now": a move started
  // earlier in this same loop pass sets moveStart after "now" was taken, and
  // now - moveStart would wrap around to a huge number and trip the timeout.
  uint32_t elapsed = millis() - moveStart;

  bool arrived = (desired == OPEN) ? atOpen() : atClosed();
  if (arrived) {
    setMotor(SERVO_STOP);
    motion = M_IDLE;
    uint32_t took = elapsed;
    Serial.printf("Door %s in %lu ms\n", desired == OPEN ? "opened" : "closed", (unsigned long)took);
    if (moveFromLimit) learnTravel(desired, took);
    if (desired == CLOSED) closedConfirmed = true;
    else noteOpened();
    return;
  }

  if (elapsed >= moveTimeout) {
    enterFault(desired == OPEN ? "jammed while opening" : "jammed while closing");
    return;
  }

  setMotor(desired == OPEN ? SERVO_OPEN : SERVO_CLOSE);
}

// ---- Door hold: re-close a closed door that has been pushed open ----------------
void startHoldAttempt(uint32_t now) {
  holdAttempts++;
  hold = H_DRIVE;
  holdTimer = now;
  setMotor(SERVO_CLOSE);
  Serial.printf("Door pushed open - re-close attempt %u\n", holdAttempts);
}

void updateHold(uint32_t now) {
  if (motion != M_IDLE || faultActive || desired != CLOSED || !closedConfirmed) {
    if (hold == H_DRIVE && motion == M_IDLE) setMotor(SERVO_STOP);
    hold = H_IDLE;
    return;
  }

  switch (hold) {
    case H_IDLE:
      if (!atClosed()) {
        hold = H_CONFIRM;
        holdTimer = now;
      }
      break;

    case H_CONFIRM:
      if (atClosed()) hold = H_IDLE;
      else if (now - holdTimer >= HOLD_CONFIRM_MS) {
        holdAttempts = 0;
        startHoldAttempt(now);
      }
      break;

    case H_DRIVE:
      if (atClosed()) {
        setMotor(SERVO_STOP);
        hold = H_IDLE;
        holdAttempts = 0;
        Serial.println("Door re-closed");
      } else if (now - holdTimer >= HOLD_ATTEMPT_MS) {
        setMotor(SERVO_STOP);
        if (holdAttempts > HOLD_RETRIES) {
          enterFault("pushed open and could not be re-closed");
        } else {
          hold = H_BACKOFF;
          holdTimer = now;
          Serial.printf("Re-close failed, retrying in %lu s\n",
                        (unsigned long)(HOLD_BACKOFF_MS[holdAttempts - 1] / 1000));
        }
      } else {
        setMotor(SERVO_CLOSE);
      }
      break;

    case H_BACKOFF:
      if (atClosed()) {
        hold = H_IDLE;
        holdAttempts = 0;
        Serial.println("Door closed on its own");
      } else if (now - holdTimer >= HOLD_BACKOFF_MS[holdAttempts - 1]) {
        startHoldAttempt(now);
      }
      break;
  }
}

void checkSwitchFault() {
  if (atOpen() && atClosed()) {
    if (!(faultActive && switchFault)) {
      enterFault("both limit switches are active - check the wiring");
      switchFault = true;
    }
  }
}

// A fault clears itself if the door ends up at the requested switch anyway
// (e.g. someone shuts it by hand).
void updateFaultClear() {
  if (!faultActive) return;
  if (atOpen() && atClosed()) return;
  bool atTarget = (desired == OPEN) ? atOpen() : atClosed();
  if (atTarget) {
    faultActive = false;
    switchFault = false;
    faultText[0] = 0;
    if (desired == CLOSED) closedConfirmed = true;
    Serial.println("Fault cleared - door is back in position");
  }
}

// ===================================================================================
// Button
// ===================================================================================
void triggerFeed(uint32_t now) {
  Serial.println("Long press - feed Idli");
  feedDev->updateAndReportParam(FEED_PARAM, false);  // "open" -> Alexa routine trigger
  feedPulse = true;
  feedPulseStart = now;
  feedsToday++;
  saveCounters();
  countersDirty = true;
  requestDoor(CLOSED, "button long press");
}

void updateFeedPulse(uint32_t now) {
  if (feedPulse && now - feedPulseStart >= FEED_PULSE_MS) {
    feedPulse = false;
    feedDev->updateAndReportParam(FEED_PARAM, true);  // back to "closed"
  }
}

void updateButton(uint32_t now) {
  bool down = (button.stable == LOW);

  if (down && !btnDown) {
    btnDown = true;
    btnStuck = false;
    btnIgnore = false;
    btnPressStart = now;
  } else if (down && btnDown) {
    if (!btnStuck && now - btnPressStart >= STUCK_BUTTON_MS) {
      btnStuck = true;
      btnIgnore = true;
      sendAlert("Idli's door button has been held for over 10 s. Is something resting on it? Ignoring it until it's released.");
    }
  } else if (!down && btnDown) {
    btnDown = false;
    uint32_t held = now - btnPressStart;

    if (btnIgnore) {
      btnIgnore = false;
      Serial.println("Button released - press ignored (stuck or held at power-up)");
      return;
    }
    if (buttonLocked) {
      Serial.println("Button press ignored - button is locked");
      return;
    }
    if (held >= LONG_PRESS_MS) triggerFeed(now);
    else if (held >= SHORT_PRESS_MIN_MS) requestDoor(desired == OPEN ? CLOSED : OPEN, "button");
  }
}

// ===================================================================================
// RainMaker
// ===================================================================================
// Runs on the RainMaker task: only copy values into flags here, loop() acts on them.
void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx) {
  const char *dev = device->getDeviceName();
  const char *p = param->getParamName();

  if (strcmp(dev, DOOR_DEVICE) == 0 && strcmp(p, DOOR_PARAM) == 0) {
    int8_t t = (val.val.i > 0) ? OPEN : CLOSED;
    int normalized = (t == OPEN) ? 100 : 0;  // slider values in between snap to 0 / 100
    rmDoorTarget = t;
    rmDoorPending = true;
    param->updateAndReport(value(normalized));
    lastReportedPos = normalized;
    return;
  }

  if (strcmp(dev, SETTINGS_DEVICE) == 0) {
    if (strcmp(p, P_POWERUP) == 0 && val.val.s) {
      for (uint8_t i = 0; i < 3; i++) {
        if (strcmp(val.val.s, POWERUP_MODES[i]) == 0) {
          rmPowerupMode = i;
          rmPowerupPending = true;
          param->updateAndReport(val);
        }
      }
    } else if (strcmp(p, P_LOCK) == 0) {
      rmLockValue = val.val.b;
      rmLockPending = true;
      param->updateAndReport(val);
    } else if (strcmp(p, P_RESET_TRAVEL) == 0) {
      if (val.val.b) rmResetTravelPending = true;
      param->updateAndReport(value(false));  // acts like a button
    }
  }
}

void handleRainMakerInputs() {
  if (rmDoorPending) {
    rmDoorPending = false;
    requestDoor(rmDoorTarget, "RainMaker/Alexa");
  }
  if (rmPowerupPending) {
    rmPowerupPending = false;
    powerupMode = rmPowerupMode;
    prefs.putUChar("powerup", powerupMode);
    Serial.printf("Power-up behavior: %s\n", POWERUP_MODES[powerupMode]);
  }
  if (rmLockPending) {
    rmLockPending = false;
    buttonLocked = rmLockValue;
    prefs.putBool("btnLock", buttonLocked);
    Serial.printf("Button %s\n", buttonLocked ? "locked" : "unlocked");
  }
  if (rmResetTravelPending) {
    rmResetTravelPending = false;
    resetTravel();
  }
}

int computeReportedPos() {
  if (motion == M_MOVING) return desired == OPEN ? 100 : 0;
  if (!faultActive && desired == CLOSED && closedConfirmed) return 0;  // held closed
  if (atOpen() && !atClosed()) return 100;
  if (atClosed() && !atOpen()) return 0;
  return 50;  // stopped between the switches
}

void computeStatus(char *buf, size_t n) {
  if (faultActive) snprintf(buf, n, "FAULT: %s", faultText);
  else if (motion == M_MOVING) snprintf(buf, n, "%s", desired == OPEN ? "Opening" : "Closing");
  else if (hold == H_DRIVE) snprintf(buf, n, "Pushed open, re-closing (attempt %u)", holdAttempts);
  else if (hold == H_BACKOFF) snprintf(buf, n, "Pushed open, retrying soon");
  else if (hold == H_CONFIRM) snprintf(buf, n, "Closed");
  else if (atOpen()) snprintf(buf, n, "Open");
  else if (atClosed()) snprintf(buf, n, "Closed");
  else snprintf(buf, n, "Partly open");
}

// Report only what changed, to stay well inside RainMaker's message limits.
void updateReporting() {
  int pos = computeReportedPos();
  if (pos != lastReportedPos) {
    lastReportedPos = pos;
    doorDev->updateAndReportParam(DOOR_PARAM, pos);
  }

  char status[80];
  computeStatus(status, sizeof(status));
  if (strcmp(status, lastStatus) != 0) {
    strcpy(lastStatus, status);
    settingsDev->updateAndReportParam(P_STATUS, status);
    Serial.printf("Status: %s\n", status);
  }

  if (countersDirty) {
    countersDirty = false;
    settingsDev->updateAndReportParam(P_OPENS, opensToday);
    settingsDev->updateAndReportParam(P_FEEDS, feedsToday);
    settingsDev->updateAndReportParam(P_LAST_OPENED, lastOpened);
  }

  if (travelDirty) {
    travelDirty = false;
    settingsDev->updateAndReportParam(P_OPEN_TIME, travel[OPEN].baseline);
    settingsDev->updateAndReportParam(P_CLOSE_TIME, travel[CLOSED].baseline);
  }
}

// Reset the daily counters at local midnight (time zone comes from the RainMaker app).
void updateDay() {
  struct tm t;
  if (!localNow(t)) return;
  int key = (t.tm_year + 1900) * 1000 + t.tm_yday;
  if (key != counterDay) {
    if (counterDay != -1) {
      opensToday = 0;
      feedsToday = 0;
    }
    counterDay = key;
    saveCounters();
    countersDirty = true;
  }
}

// RainMaker turns off the core's automatic OTA confirmation, so confirm a new image
// once it has run for a while with Wi-Fi up. Otherwise it reverts on the next reboot.
void confirmOtaIfPending(uint32_t now) {
  static bool checked = false;
  if (checked || now < OTA_CONFIRM_AFTER_MS || WiFi.status() != WL_CONNECTED) return;
  checked = true;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("OTA image confirmed");
  }
}

void sysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
      Serial.printf("\nProvisioning started on BLE as \"%s\" (%s)\n", service_name,
                    pop ? pop : "no PoP needed");
      WiFiProv.printQR(service_name, pop, "ble");
      break;
    case ARDUINO_EVENT_PROV_INIT: WiFiProv.disableAutoStop(10000); break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS: WiFiProv.endProvision(); break;
    default:;
  }
}

// ===================================================================================
// Setup
// ===================================================================================
void setup() {
  Serial.begin(115200);
  Serial.printf("\nIdli's Crate Door v%s\n", FW_VERSION);

  rgbLedWrite(PIN_LED, 0, 0, 0);  // LED is hidden; keep it off

  pinMode(PIN_LIM_OPEN, INPUT_PULLUP);
  pinMode(PIN_LIM_CLOSE, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  delay(20);
  initInput(limOpen);
  initInput(limClose);
  initInput(button);
  btnDown = (button.stable == LOW);  // a button already held at power-up is ignored
  btnIgnore = btnDown;               // when released (the stuck alert still applies)
  btnPressStart = millis();

  servo.attach(PIN_SERVO);
  setMotor(SERVO_STOP);

  // ---- Saved settings and history ----
  prefs.begin("idlidoor", false);
  powerupMode = prefs.getUChar("powerup", PU_STAY);
  if (powerupMode > PU_LAST) powerupMode = PU_STAY;
  buttonLocked = prefs.getBool("btnLock", false);
  loadTravel(OPEN);
  loadTravel(CLOSED);
  opensToday = prefs.getInt("opens", 0);
  feedsToday = prefs.getInt("feeds", 0);
  counterDay = prefs.getInt("day", -1);
  prefs.getString("lastOpen", lastOpened, sizeof(lastOpened));
  if (lastOpened[0] == 0) strcpy(lastOpened, "--");

  // ---- Where is the door, and what should it do at power-up? ----
  if (atOpen() && !atClosed()) desired = OPEN;
  else if (atClosed() && !atOpen()) desired = CLOSED;
  else desired = OPEN;  // between switches: the first short press closes it
  closedConfirmed = atClosed() && !atOpen();

  int8_t bootTarget = -1;
  if (powerupMode == PU_CLOSE) bootTarget = CLOSED;
  else if (powerupMode == PU_LAST) {
    int8_t last = prefs.getChar("lastDesired", -1);
    if (last == OPEN || last == CLOSED) bootTarget = last;
  }
  int initialPos = (bootTarget >= 0) ? bootTarget * 100 : computeReportedPos();
  lastReportedPos = initialPos;
  Serial.printf("Power-up: door reads %d, behavior \"%s\"\n", computeReportedPos(), POWERUP_MODES[powerupMode]);

  // ---- RainMaker node and devices ----
  Node my_node = RMaker.initNode("ESP RainMaker Node");

  doorDev = new Device(DOOR_DEVICE, "esp.device.blinds-external", NULL);
  doorDev->addNameParam();
  Param doorPos(DOOR_PARAM, "esp.param.blinds-position", value(initialPos), PROP_FLAG_READ | PROP_FLAG_WRITE);
  doorPos.addUIType(ESP_RMAKER_UI_SLIDER);
  doorPos.addBounds(value(0), value(100), value(1));
  doorDev->addParam(doorPos);
  doorDev->addCb(write_callback);
  my_node.addDevice(*doorDev);

  feedDev = new Device(FEED_DEVICE, "esp.device.contact-sensor", NULL);
  feedDev->addNameParam();
  Param feedState(FEED_PARAM, "esp.param.contact-detection-state", value(true), PROP_FLAG_READ);
  feedDev->addParam(feedState);
  my_node.addDevice(*feedDev);

  settingsDev = new Device(SETTINGS_DEVICE, "idli.device.settings", NULL);
  settingsDev->addNameParam();

  Param pStatus(P_STATUS, "idli.param.status", value("Starting"), PROP_FLAG_READ);
  pStatus.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pStatus);

  Param pPowerup(P_POWERUP, "idli.param.powerup", value(POWERUP_MODES[powerupMode]), PROP_FLAG_READ | PROP_FLAG_WRITE);
  pPowerup.addUIType(ESP_RMAKER_UI_DROPDOWN);
  pPowerup.addValidStrList(POWERUP_MODES, 3);
  settingsDev->addParam(pPowerup);

  Param pLock(P_LOCK, "idli.param.button-lock", value(buttonLocked), PROP_FLAG_READ | PROP_FLAG_WRITE);
  pLock.addUIType(ESP_RMAKER_UI_TOGGLE);
  settingsDev->addParam(pLock);

  Param pOpens(P_OPENS, "idli.param.opens", value(opensToday), PROP_FLAG_READ);
  pOpens.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pOpens);

  Param pFeeds(P_FEEDS, "idli.param.feeds", value(feedsToday), PROP_FLAG_READ);
  pFeeds.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pFeeds);

  Param pLastOpened(P_LAST_OPENED, "idli.param.last-opened", value((const char *)lastOpened), PROP_FLAG_READ);
  pLastOpened.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pLastOpened);

  Param pOpenTime(P_OPEN_TIME, "idli.param.open-time", value(travel[OPEN].baseline), PROP_FLAG_READ);
  pOpenTime.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pOpenTime);

  Param pCloseTime(P_CLOSE_TIME, "idli.param.close-time", value(travel[CLOSED].baseline), PROP_FLAG_READ);
  pCloseTime.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pCloseTime);

  Param pResetTravel(P_RESET_TRAVEL, "idli.param.reset-travel", value(false), PROP_FLAG_READ | PROP_FLAG_WRITE);
  pResetTravel.addUIType(ESP_RMAKER_UI_TOGGLE);
  settingsDev->addParam(pResetTravel);

  Param pFirmware(P_FIRMWARE, "idli.param.firmware", value(FW_VERSION), PROP_FLAG_READ);
  pFirmware.addUIType(ESP_RMAKER_UI_TEXT);
  settingsDev->addParam(pFirmware);

  settingsDev->addCb(write_callback);
  my_node.addDevice(*settingsDev);

  // ---- RainMaker services ----
  RMaker.enableOTA(OTA_USING_TOPICS);
  RMaker.enableTZService();
  RMaker.enableSchedule();
  RMaker.enableScenes();
  initAppInsights();
  RMaker.enableSystemService(SYSTEM_SERV_FLAGS_ALL, 2, 2, 2);

  WiFiProv.initProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM);
  RMaker.start();
  WiFi.onEvent(sysProvEvent);  // called from another task
  WiFiProv.beginProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM,
                          NETWORK_PROV_SECURITY_1, pop, service_name);

  if (bootTarget >= 0) requestDoor(bootTarget, "power-up behavior");
}

// ===================================================================================
// Loop
// ===================================================================================
void loop() {
  uint32_t now = millis();

  readInput(limOpen, now);
  readInput(limClose, now);
  readInput(button, now);

  handleRainMakerInputs();
  updateButton(now);
  checkSwitchFault();
  updateMotion();
  updateHold(now);
  updateFaultClear();
  updateFeedPulse(now);

  // Safety net: the motor only runs during a move or a re-close attempt.
  if (motion == M_IDLE && hold != H_DRIVE) setMotor(SERVO_STOP);

  static uint32_t lastSlowTick = 0;
  if (now - lastSlowTick >= 1000) {
    lastSlowTick = now;
    updateDay();
    confirmOtaIfPending(now);
  }

  updateReporting();
  delay(5);
}
