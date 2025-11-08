// INCLUDES
#include "pins.h"
#include "Arduino.h"
#include "TFT_eSPI.h"

// TFT OBJECT
TFT_eSPI tft = TFT_eSPI();

// PIN DEFINITIONS (Your working pins)
#define SENSOR_PIN 17
#define MODE_PIN 18
#define SET_PIN 16  // Not used yet, but defined
#define RESET_PIN 15
#define VERSION 7


// --- Counter Variables ---
// 'volatile' is crucial. It tells the compiler that these variables
// can be changed by an outside process (our interrupt) at any time.
volatile int sensorCount = 0;
volatile int modeCount = 0;
volatile int setCount = 0;

// --- Debounce timers ---
// We need this to prevent one button press from firing the interrupt 100+ times.
volatile unsigned long lastSensorTime = 0;
volatile unsigned long lastModeTime = 0;
volatile unsigned long lastSetTime = 0;
volatile unsigned long lastResetTime = 0;
long debounceDelay = 150;  // 150ms. Increase if you get "bouncy" multiple counts.
long sensorDebounceDelay = 50;


// --- RPM Calculation Variables ---
volatile unsigned long currentPulseTime = 0;
volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulseDuration = 0;
float currentRPM = 0.0;

// --- 5 sec window RPM Calculation Variables ---
float fiveSecRPM = 0.0;
float fiveSecMPH = 0.0;
unsigned long lastSmaCalcTime = 0;
int lastSensorCountForSma = 0;
#define SMA_CALC_INTERVAL 5000

// My original intent was to use the wheel circumference.
// to calculate distance.
// Example: 26-inch wheel = 26 * PI = 81.68 inches.
// 81.68 in / (12 in/ft * 5280 ft/mi) = 0.001288 miles.
//
// The actual circumference doesnt really translate to
// a realistic distance. A normal bike travels around 50-150 RPMS
// but the fan on the bike spins at ~600 RPM so you need to tune this 
// variable to make sense based on your RPMS.
// This should probably be renamed to conversion
// factor or something more generic than circumference. I've
// found this factor to work well for my bike, judged by
// 3 people, so YMMV.
#define WHEEL_CIRCUMFERENCE_MILES 0.0006

float distanceMiles = 0.0;
float currentMPH = 0.0;
float averageMPH = 0.0;
unsigned long startTime = 0;  // For the clock


#define RPM_CALC_INTERVAL 200  // 0.2 seconds
// Increase for Responsiveness, decrease for smoothness
#define SMOOTHING_FACTOR 0.1

unsigned long lastRpmCalcTime = 0;
int lastSensorCountForRpm = 0;


float instantaneousRPM = 0.0;

// INTERRUPT FUNCTIONS (ISR)
// These functions are special. They run *immediately* when the pin changes.
// They must be as fast as possible.
// "IRAM_ATTR" tells the ESP32 to store this function in fast RAM.

void IRAM_ATTR onSensorPulse() {
  if (millis() - lastSensorTime > sensorDebounceDelay) {
    sensorCount++;
    lastSensorTime = millis();
  }
}

void IRAM_ATTR onModePress() {
  if (millis() - lastModeTime > debounceDelay) {
    modeCount++;
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
    // Reset original counters
    sensorCount = 0;
    setCount = 0;
    modeCount = 0;

    distanceMiles = 0.0;
    currentMPH = 0.0;
    currentRPM = 0.0;
    averageMPH = 0.0;

    // Reset diagnostic variables
    fiveSecRPM = 0.0;
    fiveSecMPH = 0.0;



    // Resync timers and counters
    lastSensorCountForRpm = 0;  // Sync with sensorCount
    lastRpmCalcTime = millis();
    startTime = millis();       // Resets clock
    lastSensorTime = millis();  // Reset pulse timeout

    // 5 second avg Resync timers and counters
    lastSmaCalcTime = millis();  // Reset SMA timer
    lastSensorCountForSma = 0;   // Reset SMA pulse counter

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


// SETUP FUNCTION
void setup() {
  // --- Standard Setup ---
  pinMode(PWR_EN_PIN, OUTPUT);
  digitalWrite(PWR_EN_PIN, HIGH);
  Serial.begin(115200);
  Serial.println("T-HMI Interrupt Counter Started");


  startTime = millis();
  lastRpmCalcTime = millis();
  lastSensorCountForRpm = sensorCount;  // Sync at start

  // --- TFT Setup ---
  tft.begin();
  tft.setRotation(0);
  tft.setSwapBytes(true);
  setBrightness(16);

  // --- PinMode Setup ---
  // We still set them as inputs, so the pullup resistor is active.
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  pinMode(SET_PIN, INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  // --- INTERRUPT SETUP ---
  // This is the magic.
  // Call our function when the pin goes from HIGH to LOW (FALLING).
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onSensorPulse, FALLING);
  attachInterrupt(digitalPinToInterrupt(MODE_PIN), onModePress, FALLING);
  attachInterrupt(digitalPinToInterrupt(SET_PIN), onSetPress, FALLING);
  attachInterrupt(digitalPinToInterrupt(RESET_PIN), onResetPress, FALLING);

  // --- Draw the screen layout ONCE ---
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.print("Testing Mode v");
  tft.println(VERSION);
  tft.println("------------------");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Time:", 10, 70, 2);
  // tft.drawString("Rotations:", 10, 100, 2);
  // tft.drawString("RPMs:", 10, 130, 2);
  tft.drawString("Speed:", 10, 160, 2);
  tft.drawString("Avg Sp:", 10, 190, 2);
  tft.drawString("Distance:", 10, 250, 2);
  // tft.drawString("Reset:",  10, 190, 2);
}

void loop() {
  unsigned long now = millis();
  // --- Calculate Clock ---
  unsigned long elapsedTime = now - startTime;
  int hours = elapsedTime / 3600000;
  int minutes = (elapsedTime % 3600000) / 60000;
  int seconds = (elapsedTime % 60000) / 1000;
  char timeString[9];
  sprintf(timeString, "%02d:%02d:%02d", hours, minutes, seconds);

  if (now - lastRpmCalcTime >= RPM_CALC_INTERVAL) {
    int pulses = sensorCount - lastSensorCountForRpm;

    // We want to check every 200ms, but more time might have passed
    // so we need to use the actual time passed to do the calculation
    // otherwise the instantaneousRPM calc will be inflated
    unsigned long timeDelta = now - lastRpmCalcTime;

    // RPM = (pulses / 5 sec) * 60 sec/min = pulses * 12
    // Calculate the fast, instantaneous RPM
    instantaneousRPM = (float)pulses * (60000.0 / timeDelta);

    // Apply Exponential Moving Average (EMA) filter to currentRPM
    // This gives a slow-moving, smooth RPM that updates frequently.
    currentRPM = (SMOOTHING_FACTOR * instantaneousRPM) + ((1.0 - SMOOTHING_FACTOR) * currentRPM);
    // Store current state for next calculation
    lastRpmCalcTime = now;
    lastSensorCountForRpm = sensorCount;
  }

  // Calculations for 5S Moving Avg.
  if (now - lastSmaCalcTime >= SMA_CALC_INTERVAL) {
    int pulses = sensorCount - lastSensorCountForSma;

    // RPM = (pulses / 5 sec) * 60 sec/min
    fiveSecRPM = (float)pulses * (60000.0 / SMA_CALC_INTERVAL);

    // MPH = (RPM * circumference * 60 minutes/hour)
    fiveSecMPH = fiveSecRPM * WHEEL_CIRCUMFERENCE_MILES * 60.0;

    // Store current state for next calculation
    lastSmaCalcTime = now;
    lastSensorCountForSma = sensorCount;
  }

  if (now - lastSensorTime > 5000) {
    currentRPM = 0.0;
    fiveSecRPM = 0.0;
    fiveSecMPH = 0.0;
  }


  // --- Calculate Distance ---
  // (sensorCount * circumference)
  distanceMiles = sensorCount * WHEEL_CIRCUMFERENCE_MILES;

  // --- Calculate Speed ---
  // MPH = (RPM * circumference * 60 minutes/hour)
  currentMPH = currentRPM * WHEEL_CIRCUMFERENCE_MILES * 60.0;

  // --- Calculate Average Speed ---
  if (elapsedTime > 0) {
    // Distance (miles) / Time (ms / 3,600,000 ms/hr)
    averageMPH = distanceMiles / (elapsedTime / 3600000.0);
  } else {
    averageMPH = 0.0;
  }

  // --- Update Display ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // this draws a black box over all the old text effectively resetting the screen
  tft.fillRect(160, 70, 80, 210, TFT_BLACK);

  // Draw all new values
  tft.drawString(timeString, 100, 70, 2);  // Clock
  // tft.drawString(String(sensorCount), 160, 100, 2);               // Rotations
  // tft.drawString(String(currentRPM, 1), 110, 130, 2);             // RPM
  tft.drawString(String(currentMPH, 2) + " mph", 115, 160, 2);    // Speed (MPH, .00)
  tft.drawString(String(averageMPH, 2) + " mph", 115, 190, 2);    // Average Speed (MPH, .00)
  tft.drawString(String(distanceMiles, 2) + " mi", 140, 250, 2);  // Distance (Miles, .00)

  // Show the live state of the Reset pin
  // tft.drawString(String(digitalRead(RESET_PIN)), 130, 190, 2);

  // Also print to Serial for debugging
  Serial.print("Rotations ");
  Serial.print(sensorCount);
  Serial.print(" | RPMS ");
  Serial.print(sensorCount);
  // Serial.print(" | Mode ");
  // Serial.print(modeCount);
  // Serial.print(" | Set ");
  // Serial.println(setCount);

  delay(100);  // This slow delay is just to update the screen, the calculations happen in the interrupts.
}