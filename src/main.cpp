#include <Arduino.h>

// H-Bridge Driver (DRV8871) Pin Configuration
// This H-bridge controls current direction through antiparallel LEDs:
// - IN1 HIGH, IN2 LOW  = Forward current (Polarity A)
// - IN1 LOW, IN2 HIGH  = Reverse current (Polarity B)
// - IN1 LOW, IN2 LOW   = Both LOW = OFF
constexpr uint8_t PIN_DRV_IN1 = 5;     // D5 -> DRV8871 IN1
constexpr uint8_t PIN_DRV_IN2 = 6;     // D6 -> DRV8871 IN2
constexpr uint8_t PIN_MODE_BUTTON = 2; // D2 -> button to GND

// Timing configuration
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;    // Button debounce duration
constexpr uint32_t BOTH_ON_TOGGLE_US = 2500;   // Fast toggle: ~200 Hz per polarity (imperceptible to eye)
constexpr uint32_t ALT_SLOW_TOGGLE_MS = 1000;  // Visible toggle: 1000ms per polarity
constexpr uint32_t ALT_TOGGLE_MS = 250;        // Visible toggle: 250ms per polarity (perceptible alternation)
constexpr uint32_t FADE_HALF_CYCLE_MS = 15000; // 15s: even -> odd (full back-and-forth cycle is 30s)
constexpr uint32_t FADE_MIX_FRAME_US = 2000;   // 2ms frame for polarity mixing (~500 Hz)
const uint32_t PUMP_HEARTBEAT_CYCLE_MS = 1000;   // Fast heartbeat: 1s total cycle
const uint32_t PUMP_BREATHING_CYCLE_MS = 4000;   // Slow breathing: 4s total cycle
constexpr uint16_t PUMP_MIN_PERMILLE = 100;    // 10% minimum brightness
constexpr uint32_t BOTH_SLOT_US = 100;         // Slot length for A/B alternation in both-LED mode

// Operating modes for the LED strips
// - BothOnFast: Both LEDs on via rapid polarity switching (looks steady)
// - EvenOn: Only even-indexed LEDs lit (forward current only)
// - OddOn: Only odd-indexed LEDs lit (reverse current only)
// - AlternateVisibleSlow: Alternating polarities at visible rate (1000ms each)
// - AlternateVisible: Alternating polarities at visible rate (250ms each)
// - FadeEvenOdd: Slow crossfade between even and odd LEDs
// - PumpHeartbeat: Fast punchy heartbeat pulse (1s cycle)
// - PumpBreathing: Slow gentle breathing pulse (4s cycle)
// - AllOff: All LEDs off
enum class Mode : uint8_t {
  BothOnFast = 0,
  EvenOn,
  OddOn,
  AlternateVisibleSlow,
  AlternateVisible,
  FadeEvenOdd,
  PumpHeartbeat,
  PumpBreathing,
  AllOff,
  Count
};

// Global state
Mode currentMode = Mode::BothOnFast;

// Button debouncing state
bool buttonStableState = HIGH;              // Debounced button state
bool buttonLastReading = HIGH;              // Raw button reading
uint32_t buttonLastChangeMs = 0;            // Timestamp of last state change

// LED polarity control
bool polarityAActive = true;                // Track current polarity direction
uint32_t lastFastToggleUs = 0;              // Timestamp of last fast toggle (microseconds)
uint32_t lastAltToggleMs = 0;               // Timestamp of last visible toggle (milliseconds)
uint32_t fadeCycleStartMs = 0;              // Timestamp when fade cycle started
uint32_t pumpCycleStartMs = 0;              // Timestamp when pump cycle started

const char *modeToString(Mode mode) {
  switch (mode) {
    case Mode::AllOff:
      return "AllOff";
    case Mode::EvenOn:
      return "EvenOn";
    case Mode::OddOn:
      return "OddOn";
    case Mode::BothOnFast:
      return "BothOnFast";
    case Mode::AlternateVisibleSlow:
      return "AlternateVisibleSlow";
    case Mode::AlternateVisible:
      return "AlternateVisible";
    case Mode::FadeEvenOdd:
      return "FadeEvenOdd";
    case Mode::PumpHeartbeat:
      return "PumpHeartbeat";
    case Mode::PumpBreathing:
      return "PumpBreathing";
    case Mode::Count:
      return "Unknown";
  }

  return "Unknown";
}

// H-Bridge control functions

// Turn off both LED polarities (safe state - both outputs low)
void driveOff() {
  digitalWrite(PIN_DRV_IN1, LOW);
  digitalWrite(PIN_DRV_IN2, LOW);
}

// Drive polarity A: IN1 high, IN2 low (forward current through LEDs)
void drivePolarityA() {
  digitalWrite(PIN_DRV_IN1, HIGH);
  digitalWrite(PIN_DRV_IN2, LOW);
}

// Drive polarity B: IN1 low, IN2 high (reverse current through LEDs)
void drivePolarityB() {
  digitalWrite(PIN_DRV_IN1, LOW);
  digitalWrite(PIN_DRV_IN2, HIGH);
}

// Apply the current polarity state to the H-Bridge
void applyPolarityState() {
  if (polarityAActive) {
    drivePolarityA();
  } else {
    drivePolarityB();
  }
}

// Mix polarity A/B within a short time frame to crossfade perceived brightness.
// dutyApermille: 0..1000 where 1000 = full polarity A, 0 = full polarity B.
void applyMixedPolarity(uint16_t dutyApermille, uint32_t nowUs) {
  uint32_t framePosUs = nowUs % FADE_MIX_FRAME_US;
  uint32_t thresholdUs = (static_cast<uint32_t>(dutyApermille) * FADE_MIX_FRAME_US) / 1000;

  if (framePosUs < thresholdUs) {
    drivePolarityA();
  } else {
    drivePolarityB();
  }
}

// Control overall brightness for both LED polarities together.
// brightnessPermille: 0..1000 where 1000 = fully on (A/B alternated), 0 = off.
void applyBothBrightness(uint16_t brightnessPermille, uint32_t nowUs) {
  uint32_t framePosUs = nowUs % FADE_MIX_FRAME_US;
  uint32_t onWindowUs = (static_cast<uint32_t>(brightnessPermille) * FADE_MIX_FRAME_US) / 1000;

  if (framePosUs >= onWindowUs) {
    driveOff();
    return;
  }

  // While in the ON window, rapidly alternate A/B so both LED directions are lit.
  uint32_t slotIndex = nowUs / BOTH_SLOT_US;
  if ((slotIndex & 1U) == 0U) {
    drivePolarityA();
  } else {
    drivePolarityB();
  }
}

// Initialize a new operating mode and set initial polarity
void enterMode(Mode newMode) {
  currentMode = newMode;
  polarityAActive = true;        // Always start with polarity A
  lastFastToggleUs = micros();   // Reset fast toggle timer
  lastAltToggleMs = millis();    // Reset visible toggle timer
  fadeCycleStartMs = millis();   // Reset fade cycle timer
  pumpCycleStartMs = millis();   // Reset pump cycle timer

  switch (currentMode) {
    case Mode::AllOff:
      driveOff();
      break;
    case Mode::EvenOn:
      drivePolarityA();
      break;
    case Mode::OddOn:
      drivePolarityB();
      break;
    case Mode::BothOnFast:
      drivePolarityA();
      break;
    case Mode::AlternateVisibleSlow:
      drivePolarityA();
      break;
    case Mode::AlternateVisible:
      drivePolarityA();
      break;
    case Mode::FadeEvenOdd:
      // Start with even LEDs at full brightness.
      drivePolarityA();
      break;
    case Mode::PumpHeartbeat:
      // Start pump at minimum brightness.
      driveOff();
      break;
    case Mode::PumpBreathing:
      // Start pump at minimum brightness.
      driveOff();
      break;
    case Mode::Count:
      break;
  }
}

// Cycle to the next operating mode (wraps around to AllOff)
void nextMode() {
  uint8_t next = static_cast<uint8_t>(currentMode) + 1;
  if (next >= static_cast<uint8_t>(Mode::Count)) {
    next = static_cast<uint8_t>(Mode::BothOnFast);
  }
  enterMode(static_cast<Mode>(next));
}

// Debounce button input and detect press events
// Returns true on button press (transition from HIGH to LOW)
bool modeButtonPressedEvent() {
  bool reading = digitalRead(PIN_MODE_BUTTON);  // Read current button state
  uint32_t nowMs = millis();

  // Detect any change in raw reading
  if (reading != buttonLastReading) {
    buttonLastChangeMs = nowMs;
  }

  // Check if reading has been stable long enough to debounce
  if ((nowMs - buttonLastChangeMs) > BUTTON_DEBOUNCE_MS) {
    if (reading != buttonStableState) {
      buttonStableState = reading;
      // Only report press events (button pushed LOW)
      if (buttonStableState == LOW) {
        buttonLastReading = reading;
        return true;
      }
    }
  }

  buttonLastReading = reading;
  return false;
}

// Update LED output states based on current mode and timing
void updateOutputs() {
  uint32_t nowMs = millis();
  uint32_t nowUs = micros();

  switch (currentMode) {
    case Mode::AllOff:
      // Outputs already off
      break;
    case Mode::EvenOn:
      // Static polarity A - illuminates even-indexed LEDs
      break;
    case Mode::OddOn:
      // Static polarity B - illuminates odd-indexed LEDs
      break;
    case Mode::BothOnFast:
      // Fast polarity toggle (~200 Hz): appears as both LEDs always on
      if ((nowUs - lastFastToggleUs) >= BOTH_ON_TOGGLE_US) {
        lastFastToggleUs = nowUs;
        polarityAActive = !polarityAActive;
        applyPolarityState();
      }
      break;
    case Mode::AlternateVisibleSlow:
      // Slow polarity toggle (1000ms each): visible alternation between polarities
      if ((nowMs - lastAltToggleMs) >= ALT_SLOW_TOGGLE_MS) {
        lastAltToggleMs = nowMs;
        polarityAActive = !polarityAActive;
        applyPolarityState();
      }
      break;
    case Mode::AlternateVisible:
      // Slow polarity toggle (250ms each): visible alternation between polarities
      if ((nowMs - lastAltToggleMs) >= ALT_TOGGLE_MS) {
        lastAltToggleMs = nowMs;
        polarityAActive = !polarityAActive;
        applyPolarityState();
      }
      break;
    case Mode::FadeEvenOdd: {
      // 15s ramp A->B, then 15s ramp B->A.
      uint32_t elapsedMs = nowMs - fadeCycleStartMs;
      uint32_t cyclePosMs = elapsedMs % (2UL * FADE_HALF_CYCLE_MS);
      uint16_t dutyApermille = 0;

      if (cyclePosMs < FADE_HALF_CYCLE_MS) {
        // A fades down from 100% to 0% while B fades up.
        dutyApermille = static_cast<uint16_t>(((FADE_HALF_CYCLE_MS - cyclePosMs) * 1000UL) / FADE_HALF_CYCLE_MS);
      } else {
        // A fades up from 0% to 100% while B fades down.
        uint32_t returnPosMs = cyclePosMs - FADE_HALF_CYCLE_MS;
        dutyApermille = static_cast<uint16_t>((returnPosMs * 1000UL) / FADE_HALF_CYCLE_MS);
      }

      applyMixedPolarity(dutyApermille, nowUs);
      break;
    }
    case Mode::PumpHeartbeat: {
      // Fast punchy heartbeat: 1s cycle, 300ms rise with ease-in, 700ms fast drop.
      constexpr uint32_t RISE_MS = 300;
      constexpr uint16_t PUMP_RANGE_PERMILLE = 1000 - PUMP_MIN_PERMILLE;

      uint32_t elapsedMs = (nowMs - pumpCycleStartMs) % PUMP_HEARTBEAT_CYCLE_MS;
      uint16_t brightnessPermille = PUMP_MIN_PERMILLE;

      if (elapsedMs < RISE_MS) {
        // Rise: slow start, accelerating (quadratic ease-in) for punch.
        uint32_t tPermille = (elapsedMs * 1000UL) / RISE_MS;
        uint32_t easedPermille = (tPermille * tPermille) / 1000UL;
        brightnessPermille = static_cast<uint16_t>(PUMP_MIN_PERMILLE +
                                                   (PUMP_RANGE_PERMILLE * easedPermille) / 1000UL);
      } else {
        // Fall: fast linear drop from 100% to 10%.
        uint32_t fallMs = elapsedMs - RISE_MS;
        uint32_t fallDurationMs = PUMP_HEARTBEAT_CYCLE_MS - RISE_MS;
        uint32_t tPermille = (fallMs * 1000UL) / fallDurationMs;
        brightnessPermille = static_cast<uint16_t>(1000U -
                                                   (PUMP_RANGE_PERMILLE * tPermille) / 1000UL);
      }

      applyBothBrightness(brightnessPermille, nowUs);
      break;
    }
    case Mode::PumpBreathing: {
      // Slow gentle breathing: 4s cycle, 2s ease-in rise, 2s ease-out fall.
      constexpr uint32_t HALF_BREATH_MS = PUMP_BREATHING_CYCLE_MS / 2;
      constexpr uint16_t PUMP_RANGE_PERMILLE = 1000 - PUMP_MIN_PERMILLE;

      uint32_t elapsedMs = (nowMs - pumpCycleStartMs) % PUMP_BREATHING_CYCLE_MS;
      uint16_t brightnessPermille = PUMP_MIN_PERMILLE;

      if (elapsedMs < HALF_BREATH_MS) {
        // Rise: gentle ease-in (slow start, accelerates).
        uint32_t tPermille = (elapsedMs * 1000UL) / HALF_BREATH_MS;
        uint32_t easedPermille = (tPermille * tPermille) / 1000UL;
        brightnessPermille = static_cast<uint16_t>(PUMP_MIN_PERMILLE +
                                                   (PUMP_RANGE_PERMILLE * easedPermille) / 1000UL);
      } else {
        // Fall: gentle ease-out (fast start, decelerates) for smooth breathing.
        uint32_t fallMs = elapsedMs - HALF_BREATH_MS;
        uint32_t tPermille = (fallMs * 1000UL) / HALF_BREATH_MS;
        // Ease-out: 1 - (1-t)^2
        uint32_t invertedT = 1000UL - tPermille;
        uint32_t easedPermille = 1000UL - ((invertedT * invertedT) / 1000UL);
        brightnessPermille = static_cast<uint16_t>(PUMP_MIN_PERMILLE +
                                                   (PUMP_RANGE_PERMILLE * easedPermille) / 1000UL);
      }

      applyBothBrightness(brightnessPermille, nowUs);
      break;
    }
    case Mode::Count:
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // Configure I/O pins
  pinMode(PIN_DRV_IN1, OUTPUT);        // H-Bridge control output
  pinMode(PIN_DRV_IN2, OUTPUT);        // H-Bridge control output
  pinMode(PIN_MODE_BUTTON, INPUT_PULLUP);  // Button input with internal pull-up

  pinMode(13, OUTPUT);
  digitalWrite(13, LOW); 
  // Initialize in safe state
  driveOff();
  enterMode(Mode::BothOnFast);

  Serial.print("Startup mode: ");
  Serial.println(modeToString(currentMode));
}

void loop() {
  // Check for mode button press and cycle to next mode
  if (modeButtonPressedEvent()) {
    nextMode();
    Serial.print("Button press -> mode: ");
    Serial.println(modeToString(currentMode));
  }

  // Update LED outputs based on current mode and timing
  updateOutputs();
}