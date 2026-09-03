/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现文件
 * 
 * 这里实现了audio_manager.h中声明的所有功能。
 * 主要包括录音缓冲区管理、音频播放控制和流式播放。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

static void apply_pcm_volume(uint8_t* data, size_t size, int volume) {
    if (!data || volume >= 10) return;
    int16_t* samples = reinterpret_cast<int16_t*>(data);
    size_t count = size / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>((static_cast<int32_t>(samples[i]) * volume) / 10);
    }
}

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , recording_duration_sec(recording_duration_sec)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(0)
    , recording_length(0)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
{
    // 🧮 计算所需缓冲区大小
    recording_buffer_size = sample_rate * recording_duration_sec;  // 录音缓冲区（样本数）
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);  // 响应缓冲区（字节数）
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器...");
    
    // 分配录音缓冲区
    recording_buffer = (int16_t*)malloc(recording_buffer_size * sizeof(int16_t));
    if (recording_buffer == nullptr) {
        ESP_LOGE(TAG, "录音缓冲区分配失败，需要 %zu 字节", 
                 recording_buffer_size * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 录音缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             recording_buffer_size * sizeof(int16_t), (unsigned long)recording_duration_sec);
    
    // 分配响应缓冲区
    response_buffer = (int16_t*)calloc(response_buffer_size / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == nullptr) {
        ESP_LOGE(TAG, "响应缓冲区分配失败，需要 %zu 字节", response_buffer_size);
        free(recording_buffer);
        recording_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             response_buffer_size, (unsigned long)response_duration_sec);
    
    // 分配流式播放缓冲区
    streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    if (streaming_buffer == nullptr) {
        ESP_LOGE(TAG, "流式播放缓冲区分配失败，需要 %zu 字节", streaming_buffer_size);
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 流式播放缓冲区分配成功，大小: %zu 字节", streaming_buffer_size);

    streaming_stream = xStreamBufferCreateStatic(
        streaming_buffer_size, 1, streaming_buffer, &streaming_stream_storage
    );
    if (streaming_stream == nullptr) {
        ESP_LOGE(TAG, "创建流式音频队列失败");
        free(streaming_buffer);
        free(recording_buffer);
        free(response_buffer);
        streaming_buffer = nullptr;
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result = xTaskCreatePinnedToCore(
        streamingPlaybackTask, "tts_playback", 4096, this, 6,
        &streaming_task, tskNO_AFFINITY
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建TTS播放任务失败");
        streaming_stream = nullptr;
        free(streaming_buffer);
        free(recording_buffer);
        free(response_buffer);
        streaming_buffer = nullptr;
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

void AudioManager::deinit() {
    streaming_shutdown.store(true);
    if (streaming_task != nullptr) {
        xTaskNotifyGive(streaming_task);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (streaming_task != nullptr) {
            vTaskDelete(streaming_task);
            streaming_task = nullptr;
        }
    }
    streaming_stream = nullptr;

    if (recording_buffer != nullptr) {
        free(recording_buffer);
        recording_buffer = nullptr;
    }
    
    if (response_buffer != nullptr) {
        free(response_buffer);
        response_buffer = nullptr;
    }
    
    
    if (streaming_buffer != nullptr) {
        free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

// 🎙️ ========== 录音功能实现 ==========

void AudioManager::startRecording() {
    is_recording = true;
    recording_length = 0;
    ESP_LOGI(TAG, "开始录音...");
}

void AudioManager::stopRecording() {
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)", 
             recording_length, getRecordingDuration());
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording || recording_buffer == nullptr) {
        return false;
    }
    
    // 📏 检查缓冲区是否还有空间
    if (recording_length + samples > recording_buffer_size) {
        ESP_LOGW(TAG, "录音缓冲区已满（超过10秒上限）");
        return false;
    }
    
    // 💾 将新的音频数据追加到缓冲区末尾
    memcpy(&recording_buffer[recording_length], data, samples * sizeof(int16_t));
    recording_length += samples;
    
    return true;
}

const int16_t* AudioManager::getRecordingBuffer(size_t& length) const {
    length = recording_length;
    return recording_buffer;
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

bool AudioManager::isRecordingBufferFull() const {
    return recording_length >= recording_buffer_size;
}

// 🔊 ========== 音频播放功能实现 ==========

void AudioManager::startReceivingResponse() {
    response_length = 0;
    response_played = false;
}

bool AudioManager::addResponseData(const uint8_t* data, size_t size) {
    size_t samples = size / sizeof(int16_t);
    
    if (samples * sizeof(int16_t) > response_buffer_size) {
        ESP_LOGW(TAG, "响应数据过大，超过缓冲区限制");
        return false;
    }
    
    memcpy(response_buffer, data, size);
    response_length = samples;
    
    ESP_LOGI(TAG, "📦 接收到完整音频数据: %zu 字节, %zu 样本", size, samples);
    return true;
}

esp_err_t AudioManager::finishResponseAndPlay() {
    if (response_length == 0) {
        ESP_LOGW(TAG, "没有响应音频数据可播放");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "📢 播放响应音频: %zu 样本 (%.2f 秒)",
             response_length, (float)response_length / sample_rate);
    
    // 🔁 添加重试机制，确保音频可靠播放
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ 响应音频播放成功");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "❌ 音频播放失败 (第%d次尝试): %s",
                     retry_count + 1, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等100ms再试
            }
        }
    }
    
    return audio_ret;
}

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    ESP_LOGI(TAG, "播放%s...", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ %s播放成功", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}


// 🌊 ========== 流式播放功能实现 ==========

void AudioManager::startStreamingPlayback() {
    ESP_LOGI(TAG, "开始流式音频播放");
    if (!streaming_stream || !streaming_task) {
        ESP_LOGE(TAG, "流式音频队列未初始化");
        return;
    }

    // A new server stream should normally arrive after the previous one has
    // drained. Give a short tail time to finish instead of resetting live PCM.
    if (is_streaming.load()) {
        streaming_input_complete.store(true);
        xTaskNotifyGive(streaming_task);
        for (int i = 0; i < 200 && is_streaming.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (is_streaming.load()) {
            ESP_LOGW(TAG, "Previous TTS stream did not drain before next stream");
            is_streaming.store(false);
        }
    }

    xStreamBufferReset(streaming_stream);
    response_played = false;
    streaming_input_complete.store(false);
    is_streaming.store(true);
    xTaskNotifyGive(streaming_task);
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming.load() || !streaming_stream || !data || size == 0) {
        return false;
    }

    size_t sent = 0;
    while (sent < size && is_streaming.load()) {
        const size_t written = xStreamBufferSend(
            streaming_stream, data + sent, size - sent, pdMS_TO_TICKS(500)
        );
        if (written == 0) {
            ESP_LOGW(TAG, "TTS缓冲区写入超时: %zu/%zu bytes, queued=%u",
                     sent, size,
                     static_cast<unsigned>(xStreamBufferBytesAvailable(streaming_stream)));
            return false;
        }
        sent += written;
    }
    return sent == size;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming.load()) {
        return;
    }
    streaming_input_complete.store(true);
    if (streaming_task) xTaskNotifyGive(streaming_task);
    ESP_LOGI(TAG, "TTS音频接收完成，等待后台播放剩余 %u 字节",
             streaming_stream
                 ? static_cast<unsigned>(xStreamBufferBytesAvailable(streaming_stream)) : 0U);
}

void AudioManager::streamingPlaybackTask(void* arg) {
    static_cast<AudioManager*>(arg)->streamingPlaybackLoop();
}

void AudioManager::streamingPlaybackLoop() {
    // Keep the 4KB PCM work area off this task's 4KB stack.
    static uint8_t chunk[STREAMING_CHUNK_SIZE];

    while (!streaming_shutdown.load()) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (streaming_shutdown.load()) break;
        if (!is_streaming.load() || !streaming_stream) continue;

        // Wait for half a second of PCM before enabling I2S. Short responses
        // start as soon as the explicit end marker arrives.
        while (is_streaming.load() && !streaming_shutdown.load() &&
               !streaming_input_complete.load() &&
               xStreamBufferBytesAvailable(streaming_stream) < STREAMING_PREBUFFER_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        bool played_any = false;
        size_t played_bytes = 0;
        int underruns = 0;
        while (is_streaming.load() && !streaming_shutdown.load()) {
            const size_t received = xStreamBufferReceive(
                streaming_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(30)
            );
            if (received > 0) {
                apply_pcm_volume(chunk, received, volume_.load());
                const esp_err_t result = bsp_play_audio_stream(chunk, received);
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "后台流式音频播放失败: %s", esp_err_to_name(result));
                    break;
                }
                played_any = true;
                played_bytes += received;
                continue;
            }

            if (streaming_input_complete.load() &&
                xStreamBufferBytesAvailable(streaming_stream) == 0) {
                break;
            }
            ++underruns;
        }

        if (played_any) bsp_audio_stop();
        response_played = true;
        is_streaming.store(false);
        ESP_LOGI(TAG, "结束流式音频播放: %zu 字节, underrun=%d",
                 played_bytes, underruns);
    }

    streaming_task = nullptr;
    vTaskDelete(nullptr);
}
