// INCLUDES
#include "pins.h"
#include "Arduino.h"
#include "TFT_eSPI.h"

// TFT OBJECT
TFT_eSPI tft = TFT_eSPI();

#define VERSION 8

// --- UI State Management ---
enum UIState {
  UI_STATE_SPLASH,
  UI_STATE_MAIN,
  UI_STATE_TESTING
};
// We use 'volatile' because this will be changed by an interrupt
volatile UIState currentUIState = UI_STATE_SPLASH;

// --- Counter Variables ---
// 'volatile' is crucial.
volatile int sensorCount = 0;
volatile int modeCount = 0;
volatile int setCount = 0;

// --- Debounce timers ---
volatile unsigned long lastSensorTime = 0;
volatile unsigned long lastModeTime = 0;
volatile unsigned long lastSetTime = 0;
volatile unsigned long lastResetTime = 0;
long debounceDelay = 150;
long sensorDebounceDelay = 50;

// --- Mode Switching Logic ---
// These are *not* volatile because they are only used inside the main loop()
static int modePresses = 0;
static unsigned long firstModePressTime = 0;
#define MODE_PRESS_WINDOW 2000     // 2-second window to detect 3 presses
static int lastSeenModeCount = 0;  // To detect new presses from the ISR


// --- RPM Calculation Variables ---
volatile unsigned long currentPulseTime = 0;
volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulseDuration = 0;
float currentRPM = 0.0;

// Conversion factor
#define WHEEL_CIRCUMFERENCE_MILES 0.0006

float distanceMiles = 0.0;
float currentMPH = 0.0;
float averageMPH = 0.0;
unsigned long startTime = 0;

// --- Global UI Data Buffers ---
char timeString[9];


#define RPM_CALC_INTERVAL 200
#define SMOOTHING_FACTOR 0.1

unsigned long lastRpmCalcTime = 0;
int lastSensorCountForRpm = 0;
float instantaneousRPM = 0.0;

// INTERRUPT FUNCTIONS (ISR)
// These run *immediately* and are stored in RAM.
void IRAM_ATTR onSensorPulse() {
  if (millis() - lastSensorTime > sensorDebounceDelay) {
    sensorCount++;
    lastSensorTime = millis();
  }
}

void IRAM_ATTR onModePress() {
  if (millis() - lastModeTime > debounceDelay) {
    modeCount++;  // Just increment. The loop() will do the smart logic.
    lastModeTime = millis();
  }
}

void IRAM_ATTR onSetPress() {
  if (millis() - lastSetTime > debounceDelay) {
    setCount++;
    lastSetTime = millis();
  }
}

void IRAM_ATTR onResetPress() {
  if (millis() - lastResetTime > debounceDelay) {
    // Reset all metrics
    sensorCount = 0;
    setCount = 0;
    modeCount = 0;
    distanceMiles = 0.0;
    currentMPH = 0.0;
    currentRPM = 0.0;
    averageMPH = 0.0;

    // Resync timers and counters
    lastSensorCountForRpm = 0;
    lastRpmCalcTime = millis();
    startTime = millis();
    lastSensorTime = millis();

    // Reset mode switching logic
    lastSeenModeCount = 0;
    modePresses = 0;

    lastResetTime = millis();
  }
}

void setBrightness(uint8_t value) {
  static uint8_t steps = 16;
  static uint8_t _brightness = 0;
  if (_brightness == value) { return; }
  if (value > 16) { value = 16; }
  if (value == 0) {
    digitalWrite(BK_LIGHT_PIN, 0);
    delay(3);
    _brightness = 0;
    return;
  }
  if (_brightness == 0) {
    digitalWrite(BK_LIGHT_PIN, 1);
    _brightness = steps;
    delayMicroseconds(30);
  }
  int from = steps - _brightness;
  int to = steps - value;
  int num = (steps + to - from) % steps;
  for (int i = 0; i < num; i++) {
    digitalWrite(BK_LIGHT_PIN, 0);
    digitalWrite(BK_LIGHT_PIN, 1);
  }
  _brightness = value;
}

// --- Splash Screen Function ---
void drawSplashScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(20, 100);
  tft.setTextSize(3);
  tft.print("Wenoker E-Bike");
  tft.setCursor(60, 140);
  tft.setTextSize(2);
  tft.print("Initializing...");
}

// --- Main UI (Stub) ---
void drawMainUI() {
  // Text padding helps us clear the old text from the screen
  // but we dont need it for this static text.
  tft.setTextPadding(0);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(40, 120);
  tft.setTextSize(3);
  tft.print("MAIN UI SCREEN");

  // Now turn on padding for dynamic text.
  tft.setTextPadding(160);

  tft.setTextSize(2);
  tft.drawString(String(currentMPH, 2) + " mph", 10, 10, 2);
  tft.drawString(String(distanceMiles, 2) + " mi", 10, 40, 2);
  tft.drawString(timeString, 10, 70, 2);

  // Now reset text padding.
  tft.setTextPadding(0);
}

// --- Testing/Debug UI ---
void drawTestingUI() {
  // Text padding helps us clear the old text from the screen
  // but we dont need it for this static text.
  tft.setTextPadding(0);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.print("Testing Mode v");
  tft.println(VERSION);
  tft.println("------------------");  // 18 dashes * 12px wide = 216px

  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // --- Labels (Column 1) ---
  // X=10, Y's are stacked
  tft.drawString("Time:", 10, 40, 2);
  tft.drawString("Speed:", 10, 70, 2);
  tft.drawString("Avg Sp:", 10, 100, 2);
  tft.drawString("Distance:", 10, 130, 2);
  tft.drawString("RPMs:", 10, 160, 2);
  tft.drawString("Rotations:", 10, 190, 2);  // This has to be the last line (240 y max).

  // Now turn on padding for dynamic text.
  tft.setTextPadding(160);

  // --- Values (Column 2) ---
  // X=160, Y's match the labels
  tft.drawString(timeString, 160, 40, 2);  // Clock
  tft.drawString(String(currentMPH, 2) + " mph", 160, 70, 2);
  tft.drawString(String(averageMPH, 2) + " mph", 160, 100, 2);
  tft.drawString(String(distanceMiles, 2) + " mi", 160, 130, 2);
  tft.drawString(String(currentRPM, 1), 160, 160, 2);
  tft.drawString(String(sensorCount), 160, 190, 2);

  // Now reset text padding.
  tft.setTextPadding(0);
}

// --- Calculation Function ---
void calculateMetrics() {
  unsigned long now = millis();
  // --- Calculate Clock ---
  unsigned long elapsedTime = now - startTime;
  int hours = elapsedTime / 3600000;
  int minutes = (elapsedTime % 3600000) / 60000;
  int seconds = (elapsedTime % 60000) / 1000;
  sprintf(timeString, "%02d:%02d:%02d", hours, minutes, seconds);

  if (now - lastRpmCalcTime >= RPM_CALC_INTERVAL) {
    int pulses = sensorCount - lastSensorCountForRpm;
    unsigned long timeDelta = now - lastRpmCalcTime;
    instantaneousRPM = (float)pulses * (60000.0 / timeDelta);
    currentRPM = (SMOOTHING_FACTOR * instantaneousRPM) + ((1.0 - SMOOTHING_FACTOR) * currentRPM);
    lastRpmCalcTime = now;
    lastSensorCountForRpm = sensorCount;
  }

  if (now - lastSensorTime > 1500) {
    currentRPM = 0.0;
  }

  // --- Calculate Distance ---
  distanceMiles = sensorCount * WHEEL_CIRCUMFERENCE_MILES;
  // --- Calculate Speed ---
  currentMPH = currentRPM * WHEEL_CIRCUMFERENCE_MILES * 60.0;
  // --- Calculate Average Speed ---
  if (elapsedTime > 0) {
    averageMPH = distanceMiles / (elapsedTime / 3600000.0);
  } else {
    averageMPH = 0.0;
  }
}

// --- UI Toggling Function ---
void toggleUIState() {
  if (currentUIState == UI_STATE_MAIN) {
    currentUIState = UI_STATE_TESTING;
  } else {
    currentUIState = UI_STATE_MAIN;
  }
  // Force a screen clear to avoid artifacts when switching
  tft.fillScreen(TFT_BLACK);
}

// --- Input Handling Function ---
void handleInputs() {
  // Check for mode presses
  // We check if the ISR has incremented modeCount
  if (modeCount != lastSeenModeCount) {
    lastSeenModeCount = modeCount;  // Acknowledge the press
    unsigned long now = millis();

    if (modePresses == 0) {
      // This is the first press in a potential sequence
      firstModePressTime = now;
      modePresses = 1;
    } else {
      // This is a subsequent press
      if (now - firstModePressTime < MODE_PRESS_WINDOW) {
        modePresses++;
      } else {
        // Too much time has passed, reset sequence
        firstModePressTime = now;
        modePresses = 1;
      }
    }

    if (modePresses == 3) {
      // We got 3 presses! Toggle the UI.
      toggleUIState();
      modePresses = 0;  // Reset for next time
    }
  }

  // Also need a timeout for the presses
  if (modePresses > 0 && millis() - firstModePressTime > MODE_PRESS_WINDOW) {
    modePresses = 0;  // Reset the sequence
  }
}

// SETUP FUNCTION
void setup() {
  // --- Standard Setup ---
  pinMode(PWR_EN_PIN, OUTPUT);
  digitalWrite(PWR_EN_PIN, HIGH);
  Serial.begin(115200);
  Serial.println("T-HMI Interrupt Counter Started");

  startTime = millis();
  lastRpmCalcTime = millis();
  lastSensorCountForRpm = sensorCount;

  // --- TFT Setup ---
  tft.begin();
  // Set rotation to 3 for 90-deg clockwise
  tft.setRotation(3);
  tft.setSwapBytes(true);
  setBrightness(16);

  // --- Draw Splash Screen
  drawSplashScreen();
  delay(2000);  // Show splash for 2 seconds

  // --- Set initial state
  currentUIState = UI_STATE_TESTING;

  // --- PinMode Setup ---
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  pinMode(SET_PIN, INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  // --- INTERRUPT SETUP ---
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onSensorPulse, FALLING);
  attachInterrupt(digitalPinToInterrupt(MODE_PIN), onModePress, FALLING);
  attachInterrupt(digitalPinToInterrupt(SET_PIN), onSetPress, FALLING);
  attachInterrupt(digitalPinToInterrupt(RESET_PIN), onResetPress, FALLING);

  // The initial screen draw is now handled by the loop()
  tft.fillScreen(TFT_BLACK);
}


void loop() {
  // Check for user input to change modes
  handleInputs();

  // Always calculate the latest metrics
  calculateMetrics();

  // Draw the correct UI based on the current state
  // (The drawing functions are responsible for clearing the screen)
  switch (currentUIState) {
    case UI_STATE_MAIN:
      drawMainUI();
      break;
    case UI_STATE_TESTING:
      drawTestingUI();
      break;
    case UI_STATE_SPLASH:
      // This state is handled in setup()
      break;
  }

  // Also print to Serial for debugging
  Serial.print("Rotations ");
  Serial.print(sensorCount);
  Serial.print(" | RPMS ");
  Serial.print(currentRPM, 1);  // Fixed: Was printing sensorCount twice
  Serial.print(" | Mode Presses ");
  Serial.print(modePresses);
  Serial.print(" | UI State ");
  Serial.println(currentUIState);

  delay(100);  // UI update delay
}