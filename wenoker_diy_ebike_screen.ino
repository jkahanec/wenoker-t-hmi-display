// INCLUDES
#include "pins.h"
#include "Arduino.h"
#include "TFT_eSPI.h"

// TFT OBJECT
TFT_eSPI tft = TFT_eSPI();

#define VERSION 10

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

// --- Pause / Sleep / Resume Timing ---
#define PAUSE_TIMEOUT_MS 1500    // No sensor pulses for this long => auto-pause
#define SLEEP_TIMEOUT_MS 20000   // Paused this long => sleep the display
// A single stray sensor pulse (e.g. EMI from a light switch) shouldn't resume
// the workout or wake the display. Require a short burst of pulses, which is
// what real pedaling looks like, before treating it as "the user is back."
#define RESUME_BURST_PULSES 3    // Pulses required to confirm real pedaling
#define RESUME_BURST_WINDOW 1500 // ms - burst pulses must land within this window
int resumeBurstCount = 0;
unsigned long resumeBurstStartTime = 0;
int lastSensorCountForResume = 0;

// --- Mode Switching Logic ---
// These are *not* volatile because they are only used inside the main loop()
static int modePresses = 0;
static unsigned long firstModePressTime = 0;
#define MODE_PRESS_WINDOW 2000 // 2-second window to detect 3 presses
// TEMPORARY: the MODE button's wiring is currently unresponsive (raw
// modeCount never increments, confirmed via serial logging). Triple-pressing
// SET drives the UI toggle instead until MODE is physically fixed. To revert,
// swap setCount/lastSeenSetCount back to modeCount/lastSeenModeCount below.
static int lastSeenSetCount = 0; // To detect new presses from the ISR

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
    lastSeenSetCount = 0;
    modePresses = 0;

    // Reset resume-burst tracking
    lastSensorCountForResume = 0;
    resumeBurstCount = 0;

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

// --- Main UI Layout ---
#define MAIN_UI_HEADER_H 26     // Status bar height (shows PAUSED)
#define MAIN_UI_TIME_BOTTOM 130 // Bottom edge of the TIME zone

// Simplified UI: just Time, current Speed, and Distance, in large text.
// Built for users who don't want the full metrics breakdown in drawTestingUI().
void drawMainUI()
{
  int w = tft.width();
  int h = tft.height();
  int midX = w / 2;
  int leftX = w / 4;
  int rightX = (3 * w) / 4;

  // Other UI states (drawTestingUI) leave text size at 2x; force it back to 1x
  // so our layout math below is reliable regardless of draw order.
  tft.setTextSize(1);

  // --- 1. STATIC LAYOUT (drawn once; re-armed after sleep clears the screen) ---
  if (uiLayoutNeedsRedraw)
  {
    tft.fillScreen(TFT_BLACK);

    tft.drawLine(0, MAIN_UI_TIME_BOTTOM, w, MAIN_UI_TIME_BOTTOM, TFT_DARKGREY);
    tft.drawLine(midX, MAIN_UI_TIME_BOTTOM, midX, h, TFT_DARKGREY);

    tft.setTextDatum(TC_DATUM); // Top-center alignment
    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.drawString("TIME", midX, MAIN_UI_HEADER_H + 14, 2);
    tft.drawString("SPEED", leftX, MAIN_UI_TIME_BOTTOM + 8, 2);
    tft.drawString("DISTANCE", rightX, MAIN_UI_TIME_BOTTOM + 8, 2);

    uiLayoutNeedsRedraw = false; // Done drawing layout
  }

  // --- 2. STATUS BAR (redrawn every call so it always reflects pause state) ---
  uint16_t headerColor = isPaused ? TFT_ORANGE : TFT_NAVY;
  tft.fillRect(0, 0, w, MAIN_UI_HEADER_H, headerColor);
  if (isPaused)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_ORANGE);
    tft.drawString("PAUSED", midX, MAIN_UI_HEADER_H / 2, 2);
  }

  // --- 3. BIG METRICS ---
  // Middle-center datum keeps numbers centered even as digit count changes.
  // Font 7 is a 7-segment "digital clock" font (digits, ':' and '.' only).
  uint16_t valueColor = isPaused ? TFT_DARKGREY : TFT_WHITE;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(valueColor, TFT_BLACK);

  // TIME - HH:MM:SS
  tft.setTextPadding(w - 40);
  tft.drawString(timeString, midX, (MAIN_UI_HEADER_H + MAIN_UI_TIME_BOTTOM) / 2 + 14, 7);

  // SPEED (current) - MPH, 1 decimal place
  tft.setTextPadding(midX - 20);
  tft.drawString(String(currentMPH, 1), leftX, MAIN_UI_TIME_BOTTOM + 65, 7);

  // DISTANCE - miles, 2 decimal places
  tft.drawString(String(distanceMiles, 2), rightX, MAIN_UI_TIME_BOTTOM + 65, 7);

  // Reset state other UI code relies on defaults for.
  tft.setTextPadding(0);
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
// Owns every pause/sleep/resume state transition. Must be called every loop()
// iteration - even while sleeping - so the resume-burst check below keeps
// sampling sensor pulses.
void calculateMetrics()
{
  unsigned long now = millis();

  // Check for Auto-Pause (no sensor pulses for PAUSE_TIMEOUT_MS)
  // We check !isPaused so we only set the start time once
  if (!isPaused && (now - lastSensorTime > PAUSE_TIMEOUT_MS))
  {
    isPaused = true;
    pauseStartTime = now;
    // Fresh baseline so pulses from before this pause can't count toward a burst.
    lastSensorCountForResume = sensorCount;
    resumeBurstCount = 0;
  }
  else if (isPaused)
  {
    // Check for Resume: require a short burst of pulses (not just one stray
    // pulse) so things like a light switch's EMI can't restart the workout.
    int newPulses = sensorCount - lastSensorCountForResume;
    lastSensorCountForResume = sensorCount;

    if (newPulses > 0)
    {
      if (resumeBurstCount == 0 || (now - resumeBurstStartTime > RESUME_BURST_WINDOW))
      {
        // Start (or restart) the burst window
        resumeBurstStartTime = now;
        resumeBurstCount = newPulses;
      }
      else
      {
        resumeBurstCount += newPulses;
      }

      if (resumeBurstCount >= RESUME_BURST_PULSES)
      {
        isPaused = false;
        // Add the duration of that specific pause to our running total
        totalPausedDuration += (now - pauseStartTime);
        if (isSleeping)
        {
          isSleeping = false;
          setBrightness(16);          // Turn screen back on
          uiLayoutNeedsRedraw = true; // Sleep cleared the screen; redraw the layout
        }
        resumeBurstCount = 0;
      }
    }
    else if (resumeBurstCount > 0 && (now - resumeBurstStartTime > RESUME_BURST_WINDOW))
    {
      // The burst stalled before reaching the threshold - drop it so a later,
      // unrelated pulse doesn't get added to a stale count.
      resumeBurstCount = 0;
    }
  }

  // Check for Auto-Sleep (paused for SLEEP_TIMEOUT_MS)
  if (isPaused && !isSleeping && (now - pauseStartTime > SLEEP_TIMEOUT_MS))
  {
    isSleeping = true;
    setBrightness(0);          // Turn off backlight
    tft.fillScreen(TFT_BLACK); // Clear video memory
  }

  if (isSleeping)
  {
    // Still sleeping; nothing else to compute.
    return;
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
  // TEMPORARY: reading setCount here instead of modeCount - see note above.
  if (setCount != lastSeenSetCount)
  {
    lastSeenSetCount = setCount; // Acknowledge the press
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

  // calculateMetrics() owns all pause/sleep/resume transitions and must run
  // every iteration (even while sleeping) to keep sampling sensor pulses.
  calculateMetrics();

  // While sleeping, skip the UI draw and debug logging to save power.
  // calculateMetrics() above is what detects a resume burst and wakes us.
  if (isSleeping)
  {
    delay(100);
    return;
  }

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