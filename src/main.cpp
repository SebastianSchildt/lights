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
constexpr uint32_t ALT_TOGGLE_MS = 250;        // Visible toggle: 250ms per polarity (perceptible alternation)

// Operating modes for the LED strips
// - BothOnFast: Both LEDs on via rapid polarity switching (looks steady)
// - EvenOn: Only even-indexed LEDs lit (forward current only)
// - OddOn: Only odd-indexed LEDs lit (reverse current only)
// - AlternateVisible: Alternating polarities at visible rate (250ms each)
// - AllOff: All LEDs off
enum class Mode : uint8_t {
  BothOnFast = 0,
  EvenOn,
  OddOn,
  AlternateVisible,
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
    case Mode::AlternateVisible:
      return "AlternateVisible";
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

// Initialize a new operating mode and set initial polarity
void enterMode(Mode newMode) {
  currentMode = newMode;
  polarityAActive = true;        // Always start with polarity A
  lastFastToggleUs = micros();   // Reset fast toggle timer
  lastAltToggleMs = millis();    // Reset visible toggle timer

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
    case Mode::AlternateVisible:
      drivePolarityA();
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
    case Mode::AlternateVisible:
      // Slow polarity toggle (250ms each): visible alternation between polarities
      if ((nowMs - lastAltToggleMs) >= ALT_TOGGLE_MS) {
        lastAltToggleMs = nowMs;
        polarityAActive = !polarityAActive;
        applyPolarityState();
      }
      break;
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