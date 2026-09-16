#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opus.h"

static const char* TAG = "VS_OPUS_MODERN";

static constexpr int RATE = 16000;
static constexpr int CHANNELS = 1;
static constexpr int FRAME_MS = 20;
static constexpr int FRAME_SAMPLES = RATE * FRAME_MS / 1000;  // 320
static constexpr int BITRATE = 24000;
static constexpr int COMPLEXITY = 1;
static constexpr int BLOCK_SECONDS = 30;
static constexpr int BLOCKS = 10;
static constexpr int FRAMES_PER_BLOCK = BLOCK_SECONDS * 1000 / FRAME_MS;
static constexpr int MAX_PACKET = 512;

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

static void generate_speech_like_frame(int16_t* pcm, uint32_t& phase_a,
                                       uint32_t& phase_b, uint32_t& noise,
                                       uint32_t frame_index) {
    const uint32_t envelope_step = (frame_index / 25U) % 10U;
    const int32_t envelope = 5000 + static_cast<int32_t>(envelope_step) * 900;

    for (int i = 0; i < FRAME_SAMPLES; ++i) {
        phase_a += 5905580U;  // ~220 Hz @ 16 kHz
        phase_b += 9126805U;  // ~340 Hz @ 16 kHz
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

static bool ctl_ok(OpusEncoder* enc, int rc, const char* what) {
    if (rc == OPUS_OK) return true;
    ESP_LOGE(TAG, "%s failed: %d %s", what, rc, opus_strerror(rc));
    opus_encoder_destroy(enc);
    return false;
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "VisiteScribe modern micro-opus benchmark");
    ESP_LOGI(TAG,
             "config: micro-opus v0.4.1 fixed-point, 16kHz mono, 20ms, VOIP, "
             "24kbps CBR, complexity=%d", COMPLEXITY);
    ESP_LOGI(TAG,
             "memory before: internal_free=%u internal_largest=%u psram_total=%u psram_free=%u stack_hwm=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    const int encoder_size = opus_encoder_get_size(CHANNELS);
    ESP_LOGI(TAG, "encoder_state=%d bytes", encoder_size);

    int error = OPUS_OK;
    const int64_t create_start = esp_timer_get_time();
    OpusEncoder* encoder = opus_encoder_create(
        RATE, CHANNELS, OPUS_APPLICATION_VOIP, &error);
    const int64_t create_us = esp_timer_get_time() - create_start;

    if (encoder == nullptr || error != OPUS_OK) {
        ESP_LOGE(TAG, "encoder create failed after %.3fms: %d %s",
                 create_us / 1000.0, error, opus_strerror(error));
        return;
    }

    ESP_LOGI(TAG,
             "encoder created in %.3fms; internal_free=%u psram_free=%u stack_hwm=%u",
             create_us / 1000.0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    if (!ctl_ok(encoder, opus_encoder_ctl(encoder, OPUS_SET_BITRATE(BITRATE)), "bitrate")) return;
    if (!ctl_ok(encoder, opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(COMPLEXITY)), "complexity")) return;
    if (!ctl_ok(encoder, opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)), "signal")) return;
    if (!ctl_ok(encoder, opus_encoder_ctl(encoder, OPUS_SET_VBR(0)), "CBR")) return;
    if (!ctl_ok(encoder, opus_encoder_ctl(encoder, OPUS_SET_DTX(0)), "DTX")) return;
    if (!ctl_ok(encoder, opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0)), "FEC")) return;

    static int16_t pcm[FRAME_SAMPLES];
    static uint8_t packet[MAX_PACKET];

    uint32_t phase_a = 0;
    uint32_t phase_b = 0x12345678U;
    uint32_t noise = 0xA5C39E17U;

    uint64_t total_bytes = 0;
    uint64_t total_encode_us = 0;
    uint64_t total_generate_us = 0;
    uint32_t min_packet = UINT32_MAX;
    uint32_t max_packet = 0;
    int64_t min_frame_us = INT64_MAX;
    int64_t max_frame_us = 0;

    const int64_t wall_start = esp_timer_get_time();

    for (int block = 0; block < BLOCKS; ++block) {
        uint64_t block_bytes = 0;
        uint64_t block_encode_us = 0;
        uint64_t block_generate_us = 0;
        int64_t block_min_us = INT64_MAX;
        int64_t block_max_us = 0;

        for (int f = 0; f < FRAMES_PER_BLOCK; ++f) {
            const uint32_t frame_index = static_cast<uint32_t>(block * FRAMES_PER_BLOCK + f);

            int64_t t = esp_timer_get_time();
            generate_speech_like_frame(pcm, phase_a, phase_b, noise, frame_index);
            const int64_t gen_us = esp_timer_get_time() - t;
            block_generate_us += gen_us;
            total_generate_us += gen_us;

            t = esp_timer_get_time();
            const int bytes = opus_encode(
                encoder, pcm, FRAME_SAMPLES, packet, sizeof(packet));
            const int64_t enc_us = esp_timer_get_time() - t;

            if (bytes < 0) {
                ESP_LOGE(TAG, "encode failed frame=%" PRIu32 ": %d %s",
                         frame_index, bytes, opus_strerror(bytes));
                opus_encoder_destroy(encoder);
                return;
            }

            block_bytes += static_cast<uint32_t>(bytes);
            total_bytes += static_cast<uint32_t>(bytes);
            block_encode_us += enc_us;
            total_encode_us += enc_us;

            if (enc_us < block_min_us) block_min_us = enc_us;
            if (enc_us > block_max_us) block_max_us = enc_us;
            if (enc_us < min_frame_us) min_frame_us = enc_us;
            if (enc_us > max_frame_us) max_frame_us = enc_us;
            if (static_cast<uint32_t>(bytes) < min_packet) min_packet = bytes;
            if (static_cast<uint32_t>(bytes) > max_packet) max_packet = bytes;
        }

        const double encode_ms = block_encode_us / 1000.0;
        const double realtime_x = (BLOCK_SECONDS * 1000000.0) / block_encode_us;
        const double kbps = (block_bytes * 8.0) / (BLOCK_SECONDS * 1000.0);
        const double avg_frame_us = block_encode_us / static_cast<double>(FRAMES_PER_BLOCK);

        ESP_LOGI(TAG,
                 "block %d/%d audio=%ds bytes=%llu kbps=%.2f encode=%.1fms gen=%.1fms realtime=%.2fx frame_us=min:%lld avg:%.1f max:%lld psram_free=%u",
                 block + 1, BLOCKS, BLOCK_SECONDS,
                 (unsigned long long)block_bytes, kbps,
                 encode_ms, block_generate_us / 1000.0, realtime_x,
                 (long long)block_min_us, avg_frame_us, (long long)block_max_us,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const int64_t wall_us = esp_timer_get_time() - wall_start;
    const double audio_seconds = BLOCKS * BLOCK_SECONDS;
    const double realtime_x = (audio_seconds * 1000000.0) / total_encode_us;
    const double kbps = (total_bytes * 8.0) / (audio_seconds * 1000.0);
    const double avg_frame_us = total_encode_us /
        static_cast<double>(BLOCKS * FRAMES_PER_BLOCK);

    ESP_LOGI(TAG,
             "DONE audio=%.1fs frames=%d bytes=%llu kbps=%.2f encode=%.1fms gen=%.1fms wall=%.1fms realtime=%.2fx frame_us=min:%lld avg:%.1f max:%lld packet=%u..%u",
             audio_seconds, BLOCKS * FRAMES_PER_BLOCK,
             (unsigned long long)total_bytes, kbps,
             total_encode_us / 1000.0, total_generate_us / 1000.0,
             wall_us / 1000.0, realtime_x,
             (long long)min_frame_us, avg_frame_us, (long long)max_frame_us,
             (unsigned)(min_packet == UINT32_MAX ? 0 : min_packet),
             (unsigned)max_packet);

    ESP_LOGI(TAG,
             "memory after: internal_free=%u internal_largest=%u psram_free=%u stack_hwm=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    opus_encoder_destroy(encoder);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
