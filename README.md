# DIY T-HMI Wenoker Exercise Bike Display
This project is a work in progress. I replaced the screen that was in the old display on my Wenoker bike with a T-HMI ESP32-S3 and wired the buttons and fan sensor to it.

## Hardware Prerequisites

### Hardware Used
1. T-HMI ESP32-S3
2. Wenoker Air Resistance Stationary Bike, Model: [4892W](https://www.wenoker.com/products/wenoker-4892w-air-resistance-exercise-bike)

### Setup
1. Remove the display case from the stand, disconnect the sensor cable plugged into the back, and unscrew the screws on the back of the display case so that you can access the board inside.
2. Remove the old screen and board by snipping the wires<sup>a</sup> and unscrewing the four screws that hold the screen in place.<sup>b</sup>
2. Mount the T-HMI in place of the old screen.
3. Connect the wires for the sensor and the buttons to the defined [pins](#pins) using JST pigtails plugged into the arduino (the project currently uses two pigtails in the first two slots)
4. With everything connected, re-assemble the board and attach it back to the stand, reconnect the sensor cable.


Notes:
a. Be careful to leave enough wire for you to connect to the new display. I had some WAGO connectors lying around, so I stripped the wires that I cut for the buttons and sensors and use the WAGOs to connect them to the JST pigtails connected to the T-HMI. 
b. Be sure not to disconnect the panel for the buttons, you still need that. You should only be removing the screen and the board behind it.

#### Pins
Find the pin definitions at the bottom of `pins.h`. Currently, they look like:
```
// project-specific 
#define SENSOR_PIN (17)
#define MODE_PIN (18)
#define SET_PIN (16)
#define RESET_PIN (15)
```
## Board Prerequisites
Below were the configurations that worked for me. They are from the T-HMI github: https://github.com/Xinyuan-LilyGO/T-HMI 

### Board configuration
You must download and use the `ESP32S3 Dev Module` with the correct version. I tested this with `esp32 by Espressif Systems` version `2.0.17`

### Arduino IDE Settings
Set these in the `Tools` menu. These will only be available if you have correctly configured the board.
```
USB CDC On Boot: "Enabled"
CPU Frequency: "240MHz (WiFi)"
Core Debug Level: "None"
USB DFU On Boot: "Enabled (Requires USB-OTG Mode)"
Erase All Flash Before Sketch Upload: "Disabled"
Events Run On: "Core 1"
Flash Mode: "QIO 80MHz"
Flash Size: "16MB (128Mb)"
JTAG Adapter: "Integrated USB JTAG"
Arduino Runs On: "Core 1"
USB Firmware MSC On Boot: "Disabled"
Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)" PSRAM: "OPI PSRAM"
Upload Mode: "UARTO / Hardware CDC"
Upload Speed: "921600"
USB Mode: "Hardware CDC and JTAG"
```

