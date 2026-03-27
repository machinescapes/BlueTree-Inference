# BlueTree — Edge Impulse Keyword Detection

## Author - Archie Roden-Buchanan

Voice keyword detection firmware for the **Waveshare ESP32-S3-Audio** board. Listens for the keyword **"settings"** using an Edge Impulse model running on-device and logs detections to Serial.

## Hardware

| Component | Detail |
|-----------|--------|
| Board | Waveshare ESP32-S3-Audio |
| MCU | ESP32-S3 (OPI PSRAM, 16 MB flash) |
| Codecs | ES8311 (DAC) + ES7210 (ADC) |
| GPIO Expander | TCA9555 |
| I2S Pins | MCLK=12, BCK=13, WS=14, DIN=15, DOUT=16 |
| I2C Pins | SCL=10, SDA=11 |

## Repository Structure

```
BlueTree/
  bluetree_inference/                  # Arduino sketch
    bluetree_inference.ino
  bluetree-word-detection_inferencing.zip   # Edge Impulse model library (ZIP)
  AGENT_INSTRUCTIONS.md                # Internal build notes
  README.md                            # This file
```

## Prerequisites

### 1. Install arduino-cli

Download from https://arduino.github.io/arduino-cli/installation/ or use winget:

```cmd
winget install ArduinoSA.CLI
```

Verify installation:

```cmd
arduino-cli version
```

### 2. Install the ESP32 Arduino Core

```cmd
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17
```

### 3. Install Libraries

#### Edge Impulse model (from this repo)

Enable ZIP library installation, then install:

```cmd
arduino-cli config set library.enable_unsafe_install true
arduino-cli lib install --zip-path bluetree-word-detection_inferencing.zip
```

#### Audio libraries (from GitHub)

Download and extract these into your Arduino libraries folder
(`C:\Users\<YOU>\Documents\Arduino\libraries\`):

- **arduino-audio-driver** — https://github.com/pschatzmann/arduino-audio-driver
  - Download ZIP, extract to `Documents\Arduino\libraries\arduino-audio-driver-main`
- **arduino-audio-tools** — https://github.com/pschatzmann/arduino-audio-tools
  - Download ZIP, extract to `Documents\Arduino\libraries\arduino-audio-tools-main`

### 4. Patch the signal_t Namespace Conflict

The Edge Impulse SDK and the audio-driver library both define a type called `signal_t`.
Open this file in a text editor:

```
Documents\Arduino\libraries\arduino-audio-tools-main\src\AudioTools\AudioLibs\I2SCodecStream.h
```

Find this line (around line 318):

```cpp
codec_cfg.i2s.signal_type = (signal_t) info.signal_type;
```

Change it to:

```cpp
codec_cfg.i2s.signal_type = (audio_driver::signal_t) info.signal_type;
```

Save the file.

## Build

Open a terminal (Command Prompt or PowerShell) in the BlueTree directory.

```cmd
arduino-cli compile -b "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" --library "%USERPROFILE%\Documents\Arduino\libraries\arduino-audio-driver-main" --library "%USERPROFILE%\Documents\Arduino\libraries\arduino-audio-tools-main" bluetree_inference
```

Expected output:

```
Sketch uses ~522 KB (16%) of program storage space.
Global variables use ~29 KB (8%) of dynamic memory.
```

## Upload

### Find your serial port

Connect the Waveshare board via USB, then:

```cmd
arduino-cli board list
```

Look for a line with `Serial Port (USB)` — note the port name (e.g. `COM3`).

### Flash the firmware

```cmd
arduino-cli upload -b "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" -p COM3 bluetree_inference
```

Replace `COM3` with your actual port.

## Monitor

```cmd
arduino-cli monitor -p COM3 -c baudrate=115200
```

Press `Ctrl+C` to exit. Only one program can use the serial port at a time — close the monitor before uploading.

### What to expect

```
BlueTree — Edge Impulse Keyword Detection
Inferencing settings:
    Interval: 0.0625 ms
    Frame size: 16000
    Sample length: 1000 ms
    No. of classes: 3
Audio codec initialised (ES8311/ES7210), input gain set to 80%
Starting continuous inference in 2 seconds...
Recording...
Predictions (DSP: 20 ms, Classification: 2 ms, Anomaly: 0 ms):
    noise: 0.99609
    settings: 0.00000
    unknown: 0.00000
```

Say **"settings"** and you should see:

```
>>> KEYWORD DETECTED: settings (0.99) <<<
```

## Tuning

These values are in `bluetree_inference.ino`:

| Setting | Default | Effect |
|---------|---------|--------|
| `KEYWORD_THRESHOLD` | `0.60` | Minimum confidence to trigger. Lower = more sensitive, more false positives |
| `TRIGGER_COOLDOWN_MS` | `2000` | Milliseconds to ignore after a detection (prevents double-trigger) |
| `setInputVolume()` | `0.8` | Codec mic gain (0.0 to 1.0). Increase if signal is too quiet |

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `signal_t is ambiguous` compile error | Apply the patch in step 4 above |
| `Multiple libraries were found for "archie-word-detection_inferencing.h"` | Delete old copies from `Documents\Arduino\libraries\` — keep only `bluetree-word-detection_inferencing` |
| Serial monitor shows nothing | Ensure `CDCOnBoot=cdc` is in the FQBN. Try unplugging and re-plugging USB |
| `Failed to start I2S` or no audio | Verify you are using the Waveshare ESP32-S3-Audio board (not a generic ESP32-S3) |
| Keyword never triggers | Lower `KEYWORD_THRESHOLD` or increase `setInputVolume()` |
| Too many false triggers | Raise `KEYWORD_THRESHOLD` toward `0.80` |
