#include "smartrukifa_bridge.h"
#include "application.h"
#include "audio/audio_codec.h"
#include "board.h"
#include "display/display.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <cstring>

#define TAG "SmartRukifa"
// Use plain ws:// (port 80) on Cloudflare Workers to save ~24KB of precious ESP32-C3 SRAM (no 2nd mbedTLS context!)
#define SMARTRUKIFA_BRIDGE_BASE_URL "ws://smartrukifa-bridge.roomshift.workers.dev/ws/esp32"

void SmartRukifaBridge::Start() {
    if (started_.exchange(true)) {
        return;
    }
    // Famous ESP32-C3 SuperMini stability fix: cap Wi-Fi TX power to 10 dBm (40 * 0.25dBm)
    // to prevent LDO brownout spikes & ceramic antenna self-interference, and avoid deep MAX_MODEM beacon drops.
    esp_wifi_set_max_tx_power(40);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    ESP_LOGI(TAG, "Starting SmartRukifa 24/7 Cloud Bridge task (low-RAM ws:// mode)");
    xTaskCreate(BridgeTaskEntry, "smartrukifa", 3072, this, 3, &task_handle_);
}

void SmartRukifaBridge::BridgeTaskEntry(void* arg) {
    static_cast<SmartRukifaBridge*>(arg)->BridgeTaskLoop();
    vTaskDelete(nullptr);
}

void SmartRukifaBridge::FeedMicPcm(const std::vector<int16_t>& pcm) {
    if (!intercom_active_.load() || !connected_.load() || pcm.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    for (size_t i = 0; i < pcm.size(); ++i) {
        if (ring_count_ >= kRingCapacity) {
            // Overwrite oldest sample if network momentarily stalls
            ring_head_ = (ring_head_ + 1) % kRingCapacity;
            ring_count_--;
        }
        size_t tail = (ring_head_ + ring_count_) % kRingCapacity;
        ring_buf_[tail] = pcm[i];
        ring_count_++;
    }
}

bool SmartRukifaBridge::Connect() {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return false;
    }

    ws_ = network->CreateWebSocket(2);
    if (!ws_) {
        ESP_LOGW(TAG, "Failed to allocate WebSocket(2)");
        return false;
    }

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            HandleBinaryAudio(data, len);
        } else {
            HandleTextCommand(data, len);
        }
    });

    ws_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Bridge WebSocket disconnected");
        connected_.store(false);
        if (intercom_active_.load()) {
            SetIntercomActive(false);
        }
    });

    std::string mac = SystemInfo::GetMacAddress();
    ws_->SetHeader("Device-Id", mac.c_str());
    ws_->SetHeader("User-Agent", "SmartRukifa-ESP32/1.0");
    ESP_LOGI(TAG, "Connecting to %s", SMARTRUKIFA_BRIDGE_BASE_URL);

    if (!ws_->Connect(SMARTRUKIFA_BRIDGE_BASE_URL)) {
        ESP_LOGW(TAG, "Connect failed, err=%d", ws_->GetLastError());
        ws_.reset();
        connected_.store(false);
        return false;
    }

    connected_.store(true);
    ESP_LOGI(TAG, "Connected to SmartRukifa Bridge!");
    SendHeartbeat();
    return true;
}

void SmartRukifaBridge::SendHeartbeat() {
    if (!ws_ || !ws_->IsConnected()) {
        connected_.store(false);
        return;
    }

    int vol = 85;
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        vol = codec->output_volume();
    }

    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"heartbeat\",\"mac\":\"%s\",\"volume\":%d,\"intercom\":%s}",
                     SystemInfo::GetMacAddress().c_str(), vol,
                     intercom_active_.load() ? "true" : "false");
    if (n > 0) {
        ws_->Send(std::string(buf, n));
    }
}

void SmartRukifaBridge::SetIntercomActive(bool active) {
    bool prev = intercom_active_.exchange(active);
    if (prev == active) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pcm_mutex_);
        ring_head_ = 0;
        ring_count_ = 0;
    }

    // Show status on OLED without decoding OGG (saves ~16KB SRAM on ESP32-C3)
    Application::GetInstance().Schedule([active]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            if (active) {
                display->ShowNotification("Intercom Ayah Aktif", 4000);
                display->SetChatMessage("system", "Intercom Live");
            } else {
                display->ShowNotification("Intercom Selesai", 2500);
                display->SetChatMessage("system", "");
            }
        }
    });
}

void SmartRukifaBridge::HandleBinaryAudio(const char* data, size_t len) {
    if (!data || len < 2 || len > 4096) {
        return;
    }
    if (esp_get_free_heap_size() < 10000) {
        return;  // Protect ESP32-C3 SRAM from OOM
    }
    size_t sample_count = len / sizeof(int16_t);
    std::vector<int16_t> samples(sample_count);
    memcpy(samples.data(), data, sample_count * sizeof(int16_t));

    Application::GetInstance().Schedule([samples = std::move(samples)]() {
        Application::GetInstance().GetAudioService().PlayPcm(samples.data(), samples.size(), 16000);
    });
}

void SmartRukifaBridge::HandleTextCommand(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        return;
    }

    cJSON* cmd_item = cJSON_GetObjectItem(root, "command");
    if (cJSON_IsString(cmd_item)) {
        std::string cmd = cmd_item->valuestring;
        ESP_LOGI(TAG, "Received remote command: %s", cmd.c_str());

        if (cmd == "intercom_start") {
            SetIntercomActive(true);
        } else if (cmd == "intercom_stop") {
            SetIntercomActive(false);
        } else if (cmd == "set_volume") {
            cJSON* v = cJSON_GetObjectItem(root, "volume");
            if (cJSON_IsNumber(v)) {
                int vol = v->valueint;
                if (vol < 0) vol = 0;
                if (vol > 100) vol = 100;
                Application::GetInstance().Schedule([vol]() {
                    auto codec = Board::GetInstance().GetAudioCodec();
                    if (codec) codec->SetOutputVolume(vol);
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        char msg[32];
                        snprintf(msg, sizeof(msg), "Volume: %d%%", vol);
                        display->ShowNotification(msg, 2500);
                    }
                });
            }
        } else if (cmd == "oled_text" || cmd == "speak_tts") {
            cJSON* t = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(t)) {
                std::string text = t->valuestring;
                bool is_tts = (cmd == "speak_tts");
                Application::GetInstance().Schedule([text, is_tts]() {
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->ShowNotification(is_tts ? "Pesan Ayah" : "Info OLED", 3000);
                        display->SetChatMessage("system", text.c_str());
                    }
                });
            }
        } else if (cmd == "set_mode") {
            cJSON* m = cJSON_GetObjectItem(root, "mode");
            if (cJSON_IsString(m)) {
                std::string mode = m->valuestring;
                Application::GetInstance().Schedule([mode]() {
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        std::string msg = "Mode: " + mode;
                        display->ShowNotification(msg.c_str(), 3000);
                    }
                });
            }
        }
    }

    cJSON_Delete(root);
}

void SmartRukifaBridge::BridgeTaskLoop() {
    TickType_t last_hb = 0;
    int16_t send_buf[kChunkSamples];

    while (true) {
        if (!connected_.load() || !ws_ || !ws_->IsConnected()) {
            connected_.store(false);
            if (!Connect()) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            last_hb = xTaskGetTickCount();
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_hb) >= pdMS_TO_TICKS(15000)) {
            SendHeartbeat();
            last_hb = now;
        }

        if (intercom_active_.load()) {
            size_t popped = 0;
            {
                std::lock_guard<std::mutex> lock(pcm_mutex_);
                if (ring_count_ >= kChunkSamples) {
                    for (size_t i = 0; i < kChunkSamples; ++i) {
                        send_buf[i] = ring_buf_[ring_head_];
                        ring_head_ = (ring_head_ + 1) % kRingCapacity;
                    }
                    ring_count_ -= kChunkSamples;
                    popped = kChunkSamples;
                }
            }

            if (popped > 0 && ws_ && ws_->IsConnected()) {
                if (esp_get_free_heap_size() > 8192) {
                    ws_->Send(reinterpret_cast<const char*>(send_buf),
                              popped * sizeof(int16_t), true);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(15));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}
