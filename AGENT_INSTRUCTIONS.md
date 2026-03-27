# BlueTree — Edge Impulse on Waveshare ESP32-S3-Audio (handoff)

Use this document when working **only** inside the **BlueTree** repo (ZIP-based model + Edge Impulse sample). **There is no dependency on other projects** (e.g. ESP32Audio “keyword_inference_fixed”); everything needed to succeed is described below.

---

## 1. What you have in BlueTree

- **Edge Impulse Arduino library** (ZIP): install into Arduino libraries (Sketch → Include Library → Add .ZIP Library). The sketch expects something like `#include <archie-word-detection_inferencing.h>` — the exact name comes from the ZIP (match the include to the folder name inside the library).
- **Sample sketch:** `example-esp32_microphone_continuous/example-esp32_microphone_continuous.ino` — this is the **default Edge Impulse ESP32 microphone continuous** template.

**Important:** That template assumes a **generic ESP32 board I2S pinout**. It does **not** match the **Waveshare ESP32-S3-Audio** hardware this product uses.

---

## 2. Target hardware (fixed facts)

| Item | Detail |
|------|--------|
| **Board** | **Waveshare ESP32-S3-Audio** (ESP32-S3 + audio subsystem; often marketed with ES8311/ES7210 codec stack) |
| **Codecs** | **ES8311** (DAC/codec) + **ES7210** (ADC) — microphone audio enters **through the codec**, not a bare MEMS-to-I2S line like many dev boards |
| **Expander** | **TCA9555** for keys, PA enable, SD chip-select, etc. |
| **USB serial** | 115200 baud typical |

### Correct I2S pin mapping for this board (codec)

From **arduino-audio-driver** `ESP32S3AISmartSpeaker` definition (authoritative for this PCB):

| Role | GPIO |
|------|------|
| I2S MCLK | **12** |
| I2S BCK (bit clock) | **13** |
| I2S WS (LRCLK) | **14** |
| I2S DIN (mic / ADC **into** ESP32) | **15** |
| I2S DOUT (to codec / playback) | **16** |
| I2C SCL (codec + TCA9555) | **10** |
| I2C SDA | **11** |

### What the Edge Impulse sample uses (wrong for this board)

The bundled sample uses:

- `i2s_port_t` **1**
- **BCK 26, WS 32, data_in 33**

Those pins are for a **different** schematic. **Do not** expect the Waveshare board to work if you only change numbers on that sample without initializing the **ES8311/ES7210** (I2C + clocking).

---

## 3. Recommended integration path (production-quality)

**Do not** rely on the sample’s raw `i2s_driver_install` alone for this board.

1. Add dependencies (Arduino Library Manager or GitHub ZIPs):
   - **arduino-audio-driver** — `https://github.com/pschatzmann/arduino-audio-driver`
   - **arduino-audio-tools** — `https://github.com/pschatzmann/arduino-audio-tools`
2. In firmware, use **`AudioBoardStream`** with board **`ESP32S3AISmartSpeaker`** to capture **16 kHz, mono, 16-bit** (must match the Edge Impulse project). Input path: **`ADC_INPUT_LINE1`** (as used in verified Waveshare sketches).
3. Feed that PCM stream into the same **buffering + inference** pattern the Edge Impulse sample uses:
   - If the exported library uses **`run_classifier_init` + `run_classifier_continuous` + slices**, keep that API and wire **slice length / window** to the **macros in the new header** (`EI_CLASSIFIER_SLICE_SIZE`, `EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW`, etc.).
   - If the export only supports **`run_classifier`** on a full window, use **`EI_CLASSIFIER_RAW_SAMPLE_COUNT`** (or equivalent) and **one shot** per buffer — **do not guess**; read the generated `model_metadata.h` / example in the ZIP.

4. **Namespace conflict:** Edge Impulse defines `ei::signal_t`. The audio stack may pull in `signal_t` from `audio_driver`. Use **`ei::signal_t`** explicitly for the classifier signal struct.

5. **`EIDSP_QUANTIZE_FILTERBANK`:** Keep or drop per Edge Impulse docs depending on RAM; the sample comments explain tradeoffs.

---

## 4. Model goals (product context)

- **Current training focus:** keyword **`settings`** (and typical **noise** / **unknown** classes if present).
- **Firmware behavior to aim for:**
  - On **confident match** for the **intended keyword(s)** only, trigger an action (initially: **Serial log**; later: protocol to **host OS** to open/show something).
  - **Exclude** non-command classes from triggering (e.g. **`noise`**, optionally **`unknown`**), or use an **allowlist** (e.g. only `settings`).
- **Threshold** is project-specific; re-tune after deployment (very low thresholds increase false positives).

---

## 5. Build / flash (ESP32-S3)

Typical **FQBN** for this class of board (adjust partition scheme to your flash size if needed):

```text
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc
```

**arduino-cli** example:

```bash
arduino-cli compile -b "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
  --library path/to/arduino-audio-driver \
  --library path/to/arduino-audio-tools \
  path/to/your_sketch_folder
```

Use **ESP32 Arduino core** 2.x consistent with Edge Impulse testing (sample mentions 2.0.4).

---

## 6. Operational pitfalls

| Issue | Notes |
|-------|------|
| **Serial port busy** | Only one monitor/upload at a time on `/dev/cu.usbmodem*`. |
| **`while (!Serial);`** | On some setups this blocks forever if no USB host opens the port; use only if you need CDC wait, or gate with timeout. |
| **“Noise” always wins** | If you trigger on any class above threshold, **noise** will spam matches — filter labels in firmware. |
| **TFLite arena** | Follow Edge Impulse note re `EI_CLASSIFIER_ALLOCATION_STATIC` if you hit allocation failures. |

---

## 7. Checklist for the implementing agent

- [ ] Install Edge Impulse library from ZIP; **include name matches** actual library.
- [ ] Confirm **sample rate and window size** in exported headers match capture (16 kHz, etc.).
- [ ] **Do not** use sample pins **26 / 32 / 33 ` on Waveshare ESP32-S3-Audio without full codec bring-up.
- [ ] Prefer **AudioBoardStream + ESP32S3AISmartSpeaker** for microphone path.
- [ ] Preserve **`run_classifier_continuous`** (or official) API from **this** export — verify against ZIP headers.
- [ ] Add **keyword allowlist** or **noise/unknown exclusion** before any host-facing action.
- [ ] Use **`ei::signal_t`** if `signal_t` is ambiguous with audio-driver headers.

---

## 8. What this repo does *not* contain

- No prior “fixed” sketch from other repos — **this file + hardware facts + sample + ZIP** are the source of truth.
- **arduino-audio-driver** / **arduino-audio-tools** are **not** vendored here unless you add them; pull from GitHub or copy into `libraries`.
