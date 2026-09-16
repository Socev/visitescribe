#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_opus_enc.h"
#include "esp_audio_enc.h"

static const char *TAG = "VS_OPUS_ESP";

static constexpr int RATE = 16000;
static constexpr int CHANNELS = 1;
static constexpr int BITS = 16;
static constexpr int BITRATE = 24000;
static constexpr int FRAME_MS = 20;
static constexpr int BLOCK_SECONDS = 30;
static constexpr int BLOCKS_PER_COMPLEXITY = 3;
static constexpr int FRAMES_PER_BLOCK = BLOCK_SECONDS * 1000 / FRAME_MS;

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

static void generate_speech_like_frame(int16_t *pcm, int samples,
                                       uint32_t &phase_a, uint32_t &phase_b,
                                       uint32_t &noise, uint32_t frame_index) {
    const uint32_t envelope_step = (frame_index / 25U) % 10U;
    const int32_t envelope = 5000 + static_cast<int32_t>(envelope_step) * 900;
    for (int i = 0; i < samples; ++i) {
        phase_a += 5905580U;
        phase_b += 9126805U;
        const int32_t saw_a = static_cast<int32_t>(phase_a >> 16) - 32768;
        const int32_t saw_b = static_cast<int32_t>(phase_b >> 16) - 32768;
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        const int32_t n = static_cast<int16_t>(noise & 0xFFFFU);
        const int32_t sample = ((saw_a * envelope) >> 15) +
                               ((saw_b * (envelope / 3)) >> 15) +
                               (n >> 5);
        pcm[i] = clamp16(sample);
    }
}

static bool precheck() {
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PRECHECK build: cpu=%dMHz quad_psram=%d",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#ifdef CONFIG_SPIRAM_MODE_QUAD
             1
#else
             0
#endif
    );
    ESP_LOGI(TAG, "PRECHECK memory: psram_total=%u psram_free=%u",
             (unsigned)psram_total, (unsigned)psram_free);

    if (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ != 240 || psram_total < 7U * 1024U * 1024U) {
        ESP_LOGE(TAG, "PRECHECK FAILED: benchmark requires 240MHz + >=7MiB Quad PSRAM");
        return false;
    }

    void *trial = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!trial) {
        ESP_LOGE(TAG, "PRECHECK FAILED: 128KiB PSRAM allocation failed");
        return false;
    }
    free(trial);
    ESP_LOGI(TAG, "PRECHECK OK");
    return true;
}

static bool run_complexity(int complexity) {
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RATE;
    cfg.channel = CHANNELS;
    cfg.bits_per_sample = BITS;
    cfg.bitrate = BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = complexity;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;
    cfg.enable_vbr = false;

    void *encoder = nullptr;
    const int64_t create_start = esp_timer_get_time();
    const esp_audio_err_t open_rc = esp_opus_enc_open(&cfg, sizeof(cfg), &encoder);
    const int64_t create_us = esp_timer_get_time() - create_start;
    if (open_rc != ESP_AUDIO_ERR_OK || !encoder) {
        ESP_LOGE(TAG, "complexity=%d open failed rc=%d after %.3fms",
                 complexity, (int)open_rc, create_us / 1000.0);
        return false;
    }

    int in_size = 0;
    int out_size = 0;
    const esp_audio_err_t size_rc = esp_opus_enc_get_frame_size(encoder, &in_size, &out_size);
    if (size_rc != ESP_AUDIO_ERR_OK || in_size <= 0 || out_size <= 0) {
        ESP_LOGE(TAG, "complexity=%d frame-size failed rc=%d in=%d out=%d",
                 complexity, (int)size_rc, in_size, out_size);
        esp_opus_enc_close(encoder);
        return false;
    }

    const int samples = in_size / static_cast<int>(sizeof(int16_t));
    ESP_LOGI(TAG,
             "complexity=%d encoder created in %.3fms frame_in=%dB samples=%d frame_out=%dB internal_free=%u psram_free=%u stack_hwm=%u",
             complexity, create_us / 1000.0, in_size, samples, out_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    if (samples != RATE * FRAME_MS / 1000) {
        ESP_LOGE(TAG, "unexpected PCM frame: expected %d samples, got %d",
                 RATE * FRAME_MS / 1000, samples);
        esp_opus_enc_close(encoder);
        return false;
    }

    auto *pcm = static_cast<int16_t *>(heap_caps_malloc(in_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *packet = static_cast<uint8_t *>(heap_caps_malloc(out_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!pcm || !packet) {
        ESP_LOGE(TAG, "buffer allocation failed in=%p out=%p", pcm, packet);
        free(pcm);
        free(packet);
        esp_opus_enc_close(encoder);
        return false;
    }

    uint32_t phase_a = 0;
    uint32_t phase_b = 0x12345678U;
    uint32_t noise = 0xA5C39E17U;
    uint64_t total_encode_us = 0;
    uint64_t total_gen_us = 0;
    uint64_t total_bytes = 0;
    int64_t global_min_us = INT64_MAX;
    int64_t global_max_us = 0;

    for (int block = 0; block < BLOCKS_PER_COMPLEXITY; ++block) {
        uint64_t block_encode_us = 0;
        uint64_t block_gen_us = 0;
        uint64_t block_bytes = 0;
        int64_t block_min_us = INT64_MAX;
        int64_t block_max_us = 0;

        for (int f = 0; f < FRAMES_PER_BLOCK; ++f) {
            const uint32_t frame_index = static_cast<uint32_t>(block * FRAMES_PER_BLOCK + f);
            int64_t t = esp_timer_get_time();
            generate_speech_like_frame(pcm, samples, phase_a, phase_b, noise, frame_index);
            const int64_t gen_us = esp_timer_get_time() - t;

            esp_audio_enc_in_frame_t in_frame = {};
            in_frame.buffer = reinterpret_cast<uint8_t *>(pcm);
            in_frame.len = static_cast<uint32_t>(in_size);
            esp_audio_enc_out_frame_t out_frame = {};
            out_frame.buffer = packet;
            out_frame.len = static_cast<uint32_t>(out_size);

            t = esp_timer_get_time();
            const esp_audio_err_t rc = esp_opus_enc_process(encoder, &in_frame, &out_frame);
            const int64_t enc_us = esp_timer_get_time() - t;
            if (rc != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "complexity=%d encode failed block=%d frame=%d rc=%d",
                         complexity, block + 1, f, (int)rc);
                free(pcm);
                free(packet);
                esp_opus_enc_close(encoder);
                return false;
            }

            block_encode_us += enc_us;
            block_gen_us += gen_us;
            block_bytes += out_frame.encoded_bytes;
            if (enc_us < block_min_us) block_min_us = enc_us;
            if (enc_us > block_max_us) block_max_us = enc_us;
        }

        total_encode_us += block_encode_us;
        total_gen_us += block_gen_us;
        total_bytes += block_bytes;
        if (block_min_us < global_min_us) global_min_us = block_min_us;
        if (block_max_us > global_max_us) global_max_us = block_max_us;

        const double realtime_x = (BLOCK_SECONDS * 1000000.0) / block_encode_us;
        const double kbps = (block_bytes * 8.0) / (BLOCK_SECONDS * 1000.0);
        const double avg_frame_us = block_encode_us / static_cast<double>(FRAMES_PER_BLOCK);
        ESP_LOGI(TAG,
                 "complexity=%d block %d/%d audio=%ds bytes=%llu kbps=%.2f encode=%.1fms gen=%.1fms realtime=%.2fx frame_us=min:%lld avg:%.1f max:%lld stack_hwm=%u",
                 complexity, block + 1, BLOCKS_PER_COMPLEXITY, BLOCK_SECONDS,
                 (unsigned long long)block_bytes, kbps,
                 block_encode_us / 1000.0, block_gen_us / 1000.0, realtime_x,
                 (long long)block_min_us, avg_frame_us, (long long)block_max_us,
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const double audio_seconds = BLOCKS_PER_COMPLEXITY * BLOCK_SECONDS;
    const double realtime_x = (audio_seconds * 1000000.0) / total_encode_us;
    const double kbps = (total_bytes * 8.0) / (audio_seconds * 1000.0);
    const double avg_frame_us = total_encode_us /
        static_cast<double>(BLOCKS_PER_COMPLEXITY * FRAMES_PER_BLOCK);
    ESP_LOGI(TAG,
             "complexity=%d DONE audio=%.0fs bytes=%llu kbps=%.2f encode=%.1fms gen=%.1fms realtime=%.2fx frame_us=min:%lld avg:%.1f max:%lld internal_free=%u psram_free=%u stack_hwm=%u",
             complexity, audio_seconds, (unsigned long long)total_bytes, kbps,
             total_encode_us / 1000.0, total_gen_us / 1000.0, realtime_x,
             (long long)global_min_us, avg_frame_us, (long long)global_max_us,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    free(pcm);
    free(packet);
    esp_opus_enc_close(encoder);
    return true;
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "VisiteScribe Espressif esp_audio_codec Opus benchmark");
    ESP_LOGI(TAG, "config: esp_audio_codec 2.5.0, 16kHz mono, 20ms, VOIP, 24kbps CBR, complexity 0 then 1");
    ESP_LOGI(TAG, "memory before: internal_free=%u internal_largest=%u psram_total=%u psram_free=%u stack_hwm=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    if (!precheck()) return;
    if (!run_complexity(0)) return;
    vTaskDelay(pdMS_TO_TICKS(250));
    if (!run_complexity(1)) return;

    ESP_LOGI(TAG, "ALL DONE");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
