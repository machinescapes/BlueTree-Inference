/*
 * BlueTree — Edge Impulse keyword detection on Waveshare ESP32-S3-Audio
 *
 * Uses AudioBoardStream (arduino-audio-tools + arduino-audio-driver) to capture
 * audio through the ES8311/ES7210 codec stack, then feeds PCM into Edge Impulse
 * run_classifier_continuous().
 *
 * Hardware: Waveshare ESP32-S3-Audio (ESP32-S3 + ES8311 + ES7210 + TCA9555)
 * Model labels: noise, settings, unknown
 * Only the "settings" keyword triggers an action.
 */

// Tested with ESP32 Arduino Core 2.0.4
// If low on RAM, set to 1 to quantize the filterbank (saves ~10 KB)
#define EIDSP_QUANTIZE_FILTERBANK 0

/* ── Includes ──────────────────────────────────────────────────────────────── */

#include <archie-word-detection_inferencing.h>

// Audio driver / tools for Waveshare ESP32-S3-Audio codec bring-up
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "ESP32S3AISmartSpeaker.h"

/* ── Model constants (from model_metadata.h) ──────────────────────────────── */
//  EI_CLASSIFIER_FREQUENCY          = 16000
//  EI_CLASSIFIER_RAW_SAMPLE_COUNT   = 16000  (1 s window)
//  EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW = 4
//  EI_CLASSIFIER_SLICE_SIZE         = 4000
//  Labels: noise (0), settings (1), unknown (2)

/* ── Keyword allowlist ─────────────────────────────────────────────────────── */
static const char *KEYWORD_ALLOWLIST[] = {"settings"};
static const size_t KEYWORD_ALLOWLIST_COUNT =
    sizeof(KEYWORD_ALLOWLIST) / sizeof(KEYWORD_ALLOWLIST[0]);
static const float KEYWORD_THRESHOLD = 0.60f; // tune after deployment
static const int   KEYWORD_HITS_NEEDED = 1;   // windows above threshold to trigger
static int         keyword_hit_count = 0;
static unsigned long last_trigger_ms = 0;
static const unsigned long TRIGGER_COOLDOWN_MS = 2000; // prevent re-triggering for 2s

/* ── Audio capture ─────────────────────────────────────────────────────────── */

using namespace audio_tools;

static AudioBoardStream audioBoard(ESP32S3AISmartSpeaker);

/* ── Inference buffers ─────────────────────────────────────────────────────── */

typedef struct {
  int16_t *buffers[2];
  uint8_t buf_select;
  volatile uint8_t buf_ready;
  uint32_t buf_count;
  uint32_t n_samples;
} inference_t;

static inference_t inference;
static bool record_status = true;
static bool debug_nn = false;
static int print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

/* ── Intermediate read buffer ──────────────────────────────────────────────── */
static const uint32_t AUDIO_READ_SIZE = 1024; // samples per read chunk
static int16_t readBuffer[AUDIO_READ_SIZE];

/* ── Forward declarations ──────────────────────────────────────────────────── */
static bool microphone_inference_start(uint32_t n_samples);
static bool microphone_inference_record(void);
static int  microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr);
static void microphone_inference_end(void);

/* ═══════════════════════════════════════════════════════════════════════════ */

void setup() {
  Serial.begin(115200);

  // Timeout-guarded wait for USB CDC (avoid blocking forever on headless boot)
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) {
    delay(10);
  }

  Serial.println("BlueTree — Edge Impulse Keyword Detection");

  // ── Print model info ──
  ei_printf("Inferencing settings:\n");
  ei_printf("\tInterval: %.4f ms\n", (float)EI_CLASSIFIER_INTERVAL_MS);
  ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
  ei_printf("\tSample length: %d ms\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
  ei_printf("\tNo. of classes: %d\n",
            sizeof(ei_classifier_inferencing_categories) /
                sizeof(ei_classifier_inferencing_categories[0]));

  // ── Bring up codec via AudioBoardStream ──
  auto cfg = audioBoard.defaultConfig(RX_MODE);
  cfg.sample_rate = EI_CLASSIFIER_FREQUENCY; // 16000
  cfg.channels = 1;                          // mono
  cfg.bits_per_sample = 16;
  cfg.input_device = ADC_INPUT_LINE1;
  cfg.sd_active = false; // we don't need SD

  if (!audioBoard.begin(cfg)) {
    ei_printf("ERR: AudioBoardStream begin() failed — check codec wiring\n");
    while (true) delay(1000);
  }

  // Let the codec handle mic gain instead of blind software scaling
  audioBoard.setInputVolume(0.8f); // 0.0–1.0, maps to ES7210 gain stages
  ei_printf("Audio codec initialised (ES8311/ES7210), input gain set to 80%%\n");

  // ── Classifier init ──
  run_classifier_init();
  ei_printf("Starting continuous inference in 2 seconds...\n");
  ei_sleep(2000);

  if (!microphone_inference_start(EI_CLASSIFIER_SLICE_SIZE)) {
    ei_printf("ERR: Could not allocate audio buffer (size %d)\n",
              EI_CLASSIFIER_RAW_SAMPLE_COUNT);
    while (true) delay(1000);
  }

  ei_printf("Recording...\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */

void loop() {
  if (!microphone_inference_record()) {
    ei_printf("ERR: Failed to record audio\n");
    return;
  }

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
  signal.get_data = &microphone_audio_signal_get_data;

  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, debug_nn);
  if (r != EI_IMPULSE_OK) {
    ei_printf("ERR: Failed to run classifier (%d)\n", r);
    return;
  }

  if (++print_results >= EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW) {
    // ── Log all class scores ──
    ei_printf("Predictions (DSP: %d ms, Classification: %d ms, Anomaly: %d ms): \n",
              result.timing.dsp, result.timing.classification,
              result.timing.anomaly);
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
      ei_printf("    %s: %.5f\n", result.classification[ix].label,
                result.classification[ix].value);
    }

#if EI_CLASSIFIER_HAS_ANOMALY == 1
    ei_printf("    anomaly score: %.3f\n", result.anomaly);
#endif

    // ── Keyword trigger (allowlist + cooldown) ──
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
      if (result.classification[ix].value < KEYWORD_THRESHOLD) continue;
      for (size_t k = 0; k < KEYWORD_ALLOWLIST_COUNT; k++) {
        if (strcmp(result.classification[ix].label, KEYWORD_ALLOWLIST[k]) == 0) {
          if (millis() - last_trigger_ms > TRIGGER_COOLDOWN_MS) {
            ei_printf(">>> KEYWORD DETECTED: %s (%.2f) <<<\n",
                      result.classification[ix].label,
                      result.classification[ix].value);
            last_trigger_ms = millis();
            // TODO: send protocol message to host OS
          }
        }
      }
    }

    print_results = 0;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Audio capture task — reads from AudioBoardStream into inference buffers    */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void capture_samples(void *arg) {
  const size_t bytes_to_read = AUDIO_READ_SIZE * sizeof(int16_t);

  // Diagnostics
  unsigned long diag_last_ms = millis();
  unsigned long diag_sample_count = 0;
  bool raw_dumped = false;

  while (record_status) {
    size_t bytesRead = audioBoard.readBytes((uint8_t *)readBuffer, bytes_to_read);
    size_t samplesRead = bytesRead / sizeof(int16_t);

    if (samplesRead == 0) {
      delay(1);
      continue;
    }

    // ── Diagnostic: dump first 32 raw samples once ──
    if (!raw_dumped && samplesRead > 0) {
      ei_printf("[DIAG] Raw samples (pre-gain): ");
      for (int i = 0; i < 32 && i < (int)samplesRead; i++) {
        ei_printf("%d ", readBuffer[i]);
      }
      ei_printf("\n");
      raw_dumped = true;
    }

    // ── Diagnostic: measure actual sample rate every 5 seconds ──
    diag_sample_count += samplesRead;
    if (millis() - diag_last_ms >= 5000) {
      ei_printf("[DIAG] Actual sample rate: %lu Hz\n", diag_sample_count / 5);
      diag_sample_count = 0;
      diag_last_ms = millis();
    }

    // No blind * 8 — codec input gain handles levels now

    // Feed into double-buffer
    for (size_t i = 0; i < samplesRead; i++) {
      inference.buffers[inference.buf_select][inference.buf_count++] = readBuffer[i];
      if (inference.buf_count >= inference.n_samples) {
        inference.buf_select ^= 1;
        inference.buf_count = 0;
        inference.buf_ready = 1;
      }
    }
  }

  vTaskDelete(NULL);
}

/* ── Allocate buffers and start capture task ──────────────────────────────── */

static bool microphone_inference_start(uint32_t n_samples) {
  inference.buffers[0] = (int16_t *)malloc(n_samples * sizeof(int16_t));
  if (!inference.buffers[0]) return false;

  inference.buffers[1] = (int16_t *)malloc(n_samples * sizeof(int16_t));
  if (!inference.buffers[1]) {
    free(inference.buffers[0]);
    return false;
  }

  inference.buf_select = 0;
  inference.buf_count = 0;
  inference.n_samples = n_samples;
  inference.buf_ready = 0;

  record_status = true;
  xTaskCreate(capture_samples, "CaptureSamples", 1024 * 32, NULL, 10, NULL);

  return true;
}

/* ── Block until a full slice is ready ────────────────────────────────────── */

static bool microphone_inference_record(void) {
  if (inference.buf_ready == 1) {
    ei_printf("Warning: sample buffer overrun. "
              "Decrease EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW\n");
  }

  while (inference.buf_ready == 0) {
    delay(1);
  }

  inference.buf_ready = 0;
  return true;
}

/* ── Convert int16 slice to float for the classifier ──────────────────────── */

static int microphone_audio_signal_get_data(size_t offset, size_t length,
                                            float *out_ptr) {
  numpy::int16_to_float(&inference.buffers[inference.buf_select ^ 1][offset],
                        out_ptr, length);
  return 0;
}

/* ── Tear down ────────────────────────────────────────────────────────────── */

static void microphone_inference_end(void) {
  record_status = false;
  delay(100); // let capture task exit
  audioBoard.end();
  free(inference.buffers[0]);
  free(inference.buffers[1]);
}

#if !defined(EI_CLASSIFIER_SENSOR) || \
    EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_MICROPHONE
#error "Invalid model for current sensor."
#endif
