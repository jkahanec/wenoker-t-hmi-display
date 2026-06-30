// INCLUDES
#include "pins.h"
#include "Arduino.h"
#include "TFT_eSPI.h"

// TFT OBJECT
TFT_eSPI tft = TFT_eSPI();

#define VERSION 8

// --- UI State Management ---
enum UIState
{
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

// --- State Management ---
bool isPaused = false;
bool isSleeping = false;
bool uiLayoutNeedsRedraw = true;

// --- Time Tracking for Pause ---
unsigned long totalPausedDuration = 0; // Total time spent paused in ms
unsigned long pauseStartTime = 0;      // When the current pause started

// --- Mode Switching Logic ---
// These are *not* volatile because they are only used inside the main loop()
static int modePresses = 0;
static unsigned long firstModePressTime = 0;
#define MODE_PRESS_WINDOW 2000    // 2-second window to detect 3 presses
static int lastSeenModeCount = 0; // To detect new presses from the ISR

// --- RPM Calculation Variables ---
volatile unsigned long currentPulseTime = 0;
volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulseDuration = 0;
float currentRPM = 0.0;

// Conversion factor
#define WHEEL_CIRCUMFERENCE_MILES 0.00048

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
void IRAM_ATTR onSensorPulse()
{
  if (millis() - lastSensorTime > sensorDebounceDelay)
  {
    sensorCount++;
    lastSensorTime = millis();
  }
}

void IRAM_ATTR onModePress()
{
  if (millis() - lastModeTime > debounceDelay)
  {
    modeCount++; // Just increment. The loop() will do the smart logic.
    lastModeTime = millis();
  }
}

void IRAM_ATTR onSetPress()
{
  if (millis() - lastSetTime > debounceDelay)
  {
    setCount++;
    lastSetTime = millis();
  }
}

void IRAM_ATTR onResetPress()
{
  if (millis() - lastResetTime > debounceDelay)
  {
    // Reset all metrics
    sensorCount = 0;
    setCount = 0;
    modeCount = 0;
    distanceMiles = 0.0;
    currentMPH = 0.0;
    currentRPM = 0.0;
    averageMPH = 0.0;

    // Reset Pause/Sleep Variables
    totalPausedDuration = 0; 
    isPaused = false;         // <--- Reset state to active
    isSleeping = false;       // <--- Wake up if somehow sleeping
    setBrightness(16);

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

void setBrightness(uint8_t value)
{
  static uint8_t steps = 16;
  static uint8_t _brightness = 0;
  if (_brightness == value)
  {
    return;
  }
  if (value > 16)
  {
    value = 16;
  }
  if (value == 0)
  {
    digitalWrite(BK_LIGHT_PIN, 0);
    delay(3);
    _brightness = 0;
    return;
  }
  if (_brightness == 0)
  {
    digitalWrite(BK_LIGHT_PIN, 1);
    _brightness = steps;
    delayMicroseconds(30);
  }
  int from = steps - _brightness;
  int to = steps - value;
  int num = (steps + to - from) % steps;
  for (int i = 0; i < num; i++)
  {
    digitalWrite(BK_LIGHT_PIN, 0);
    digitalWrite(BK_LIGHT_PIN, 1);
  }
  _brightness = value;
}

// --- Splash Screen Function ---
void drawSplashScreen()
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(20, 100);
  tft.setTextSize(3);
  tft.print("Wenoker E-Bike");
  tft.setCursor(60, 140);
  tft.setTextSize(2);
  tft.print("Initializing...");
}

void drawMainUI()
{
  // --- 1. DRAW STATIC LAYOUT (Only once per screen load) ---
  if (uiLayoutNeedsRedraw) {
    // Draw Top Header Bar
    tft.fillRect(0, 0, 320, 30, TFT_NAVY); // Dark Blue Header
    
    // Draw Vertical Divider Line
    tft.drawLine(160, 30, 160, 240, TFT_DARKGREY);

    // Draw Static Labels (Font 2 is a nice small sans-serif)
    tft.setTextColor(TFT_SILVER, TFT_BLACK); // Grey text, Black background
    tft.setTextDatum(TC_DATUM); // Top Center alignment
    
    // Left Label
    tft.drawString("TIME", 80, 45, 2); // Centered in left half (0-160)
    
    // Right Label
    tft.drawString("DISTANCE", 240, 45, 2); // Centered in right half (160-320)

    // Top Right Screen Name
    tft.setTextDatum(TR_DATUM); // Top Right alignment
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("MAIN", 310, 5, 2);

    uiLayoutNeedsRedraw = false; // Done drawing layout
  }

  // --- 2. UPDATE DYNAMIC NUMBERS ---
  
  // -- PAUSE INDICATOR --
  tft.setTextDatum(TL_DATUM); // Top Left
  if (isPaused) {
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString("PAUSED", 10, 5, 2);
  } else {
    // Draw over "PAUSED" with the background color to hide it
    tft.setTextColor(TFT_NAVY, TFT_NAVY);
    tft.drawString("PAUSED", 10, 5, 2);
  }

  // -- BIG METRICS --
  // We use Middle Center datum to ensure numbers stay centered 
  // even as they grow from 1 digit to 3 digits.
  tft.setTextDatum(MC_DATUM); 

  // TIME (Left Side)
  // Font 7 is a 7-segment display font (like a digital clock) usually built-in.
  // If Font 7 is too big/glitchy, switch to Font 6 or 4.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(140); // Width to clear previous text
  tft.drawString(timeString, 80, 130, 7); 

  // DISTANCE (Right Side)
  // We format distance to 1 decimal place for readability
  tft.setTextColor(TFT_CYAN, TFT_BLACK); // Cyan pops well against black
  String distStr = String(distanceMiles, 1);
  tft.drawString(distStr, 240, 130, 7);

  // Small unit label below the number
  tft.setTextPadding(0); // Turn off padding for static text
  tft.setTextColor(TFT_SILVER, TFT_BLACK);
  tft.drawString("miles", 240, 180, 2);

  // Reset Datum to default just in case
  tft.setTextDatum(TL_DATUM);
}

// --- Testing/Debug UI ---
void drawTestingUI()
{
  // Text padding helps us clear the old text from the screen
  // but we dont need it for this static text.
  tft.setTextPadding(0);

  tft.setTextSize(2);
  tft.setCursor(0, 0);

  if (isPaused)
  {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK); // Yellow for pause
    tft.print("Testing Mode v");
    tft.print(VERSION);
    tft.println(" PAUSED");
  }
  else
  {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("Testing Mode v");
    tft.print(VERSION);
    tft.println(" ACTIVE");
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // --- Labels (Column 1) ---
  // X=10, Y's are stacked
  tft.drawString("Time:", 10, 40, 2);
  tft.drawString("Speed:", 10, 70, 2);
  tft.drawString("Avg Sp:", 10, 100, 2);
  tft.drawString("Distance:", 10, 130, 2);
  tft.drawString("RPMs:", 10, 160, 2);
  tft.drawString("Rotations:", 10, 190, 2); // This has to be the last line (240 y max).

  // Now turn on padding for dynamic text.
  tft.setTextPadding(160);

  // --- Values (Column 2) ---
  // X=160, Y's match the labels
  tft.drawString(timeString, 160, 40, 2); // Clock
  tft.drawString(String(currentMPH, 2) + " mph", 160, 70, 2);
  tft.drawString(String(averageMPH, 2) + " mph", 160, 100, 2);
  tft.drawString(String(distanceMiles, 2) + " mi", 160, 130, 2);
  tft.drawString(String(currentRPM, 1), 160, 160, 2);
  tft.drawString(String(sensorCount), 160, 190, 2);

  // Now reset text padding.
  tft.setTextPadding(0);
}

// --- Calculation Function ---
void calculateMetrics()
{
  unsigned long now = millis();

  // Check for Auto-Pause (No signal for 5 seconds)
  // We check !isPaused so we only set the start time once
  if (!isPaused && (now - lastSensorTime > 5000))
  {
    isPaused = true;
    pauseStartTime = now;
  }

  // Check for Resume (Signal received recently)
  // If we were paused, but lastSensorTime is fresh (updated by ISR), we resume.
  if (isPaused && (now - lastSensorTime < 1000))
  {
    isPaused = false;
    // Add the duration of that specific pause to our running total
    totalPausedDuration += (now - pauseStartTime);
  }
  // If sleeping, we check if we should wake up BEFORE returning
  if (isSleeping)
  {
    // The ISR updates lastSensorTime even while we sleep.
    // If the user has pedaled in the last second, WAKE UP.
    if (millis() - lastSensorTime < 1000)
    {
      isSleeping = false;
      setBrightness(16); // Turn screen back on
      // We do NOT return here. We let the code fall through
      // so calculateMetrics() can run and fix the timers.
    }
    else
    {
      // Still sleeping and no activity... stay asleep.
      delay(100);
      return;
    }
  }

// Calculate "Active" Time
// Start with total raw time minus previously accumulated pauses
unsigned long activeDuration = (now - startTime) - totalPausedDuration;

// If we are currently paused, subtract the CURRENT pause duration too
// so the timer visually stops ticking.
if (isPaused)
{
  activeDuration -= (now - pauseStartTime);
}

// --- Calculate Clock using activeDuration instead of raw elapsedTime ---
int hours = activeDuration / 3600000;
int minutes = (activeDuration % 3600000) / 60000;
int seconds = (activeDuration % 60000) / 1000;
sprintf(timeString, "%02d:%02d:%02d", hours, minutes, seconds);

if (now - lastRpmCalcTime >= RPM_CALC_INTERVAL)
{
  int pulses = sensorCount - lastSensorCountForRpm;
  unsigned long timeDelta = now - lastRpmCalcTime;
  instantaneousRPM = (float)pulses * (60000.0 / timeDelta);
  currentRPM = (SMOOTHING_FACTOR * instantaneousRPM) + ((1.0 - SMOOTHING_FACTOR) * currentRPM);
  lastRpmCalcTime = now;
  lastSensorCountForRpm = sensorCount;
}

if (now - lastSensorTime > 1500)
{
  currentRPM = 0.0;
}

// --- Calculate Distance ---
distanceMiles = sensorCount * WHEEL_CIRCUMFERENCE_MILES;
// --- Calculate Speed ---
currentMPH = currentRPM * WHEEL_CIRCUMFERENCE_MILES * 60.0;
// --- Calculate Average Speed ---
if (activeDuration > 0)
{
  averageMPH = distanceMiles / (activeDuration / 3600000.0);
}
else
{
  averageMPH = 0.0;
}
}

// --- UI Toggling Function ---
void toggleUIState()
{
  if (currentUIState == UI_STATE_MAIN)
  {
    currentUIState = UI_STATE_TESTING;
  }
  else
  {
    currentUIState = UI_STATE_MAIN;
  }
  
  // Force a screen clear
  tft.fillScreen(TFT_BLACK);
  
  // TRIGGER A LAYOUT REDRAW
  uiLayoutNeedsRedraw = true; 
}

// --- Input Handling Function ---
void handleInputs()
{
  // Check for mode presses
  // We check if the ISR has incremented modeCount
  if (modeCount != lastSeenModeCount)
  {
    lastSeenModeCount = modeCount; // Acknowledge the press
    unsigned long now = millis();

    if (modePresses == 0)
    {
      // This is the first press in a potential sequence
      firstModePressTime = now;
      modePresses = 1;
    }
    else
    {
      // This is a subsequent press
      if (now - firstModePressTime < MODE_PRESS_WINDOW)
      {
        modePresses++;
      }
      else
      {
        // Too much time has passed, reset sequence
        firstModePressTime = now;
        modePresses = 1;
      }
    }

    if (modePresses == 3)
    {
      // We got 3 presses! Toggle the UI.
      toggleUIState();
      modePresses = 0; // Reset for next time
    }
  }

  // Also need a timeout for the presses
  if (modePresses > 0 && millis() - firstModePressTime > MODE_PRESS_WINDOW)
  {
    modePresses = 0; // Reset the sequence
  }
}

// SETUP FUNCTION
void setup()
{
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
  delay(2000); // Show splash for 2 seconds

  // --- Set initial state
  currentUIState = UI_STATE_MAIN;

  // for testing
  // currentUIState = UI_STATE_TESTING;

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

void loop()
{
  handleInputs();

  // --- Sleep Logic ---
  // If paused for > 60 seconds (60000ms) and not yet sleeping
  if (isPaused && !isSleeping && (millis() - pauseStartTime > 20000))
  {
    isSleeping = true;
    setBrightness(0);          // Turn off backlight
    tft.fillScreen(TFT_BLACK); // Clear video memory
  }

  // If sleeping, do NOTHING else.
  // The ISR (onSensorPulse) will update 'lastSensorTime',
  // which causes the logic in Step 2 to wake us up.
  // If sleeping, we need to check if we should wake up!
  if (isSleeping)
  {
    // The ISR updates lastSensorTime even while we sleep.
    // If the user has pedaled in the last second, WAKE UP.
    if (millis() - lastSensorTime < 1000)
    {
      isSleeping = false;
      setBrightness(16); // Turn screen back on
      // We do NOT return here. We let the code fall through to
      // calculateMetrics() so it can fix the pause timer.
    }
    else
    {
      // Still sleeping and no activity... stay asleep.
      delay(100);
      return;
    }
  }

  calculateMetrics();

  // Draw the correct UI based on the current state
  // (The drawing functions are responsible for clearing the screen)
  switch (currentUIState)
  {
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
  Serial.print(currentRPM, 1); // Fixed: Was printing sensorCount twice
  Serial.print(" | Mode Presses ");
  Serial.print(modePresses);
  Serial.print(" | UI State ");
  Serial.println(currentUIState);

  delay(100); // UI update delay
}