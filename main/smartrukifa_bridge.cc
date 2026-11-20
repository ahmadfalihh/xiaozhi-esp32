#include "smartrukifa_bridge.h"
#include "application.h"
#include "assets/lang_config.h"
#include "audio/audio_codec.h"
#include "board.h"
#include "display/display.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <cstring>
#include <vector>

#define TAG "SmartRukifa"
#define SMARTRUKIFA_BRIDGE_BASE_URL "wss://smartrukifa-bridge.roomshift.workers.dev/ws/esp32"

void SmartRukifaBridge::Start() {
    if (started_.exchange(true)) {
        return;
    }
    ESP_LOGI(TAG, "Starting SmartRukifa 24/7 Cloud Bridge task");
    xTaskCreate(BridgeTaskEntry, "smartrukifa", 4096, this, 3, &task_handle_);
}

void SmartRukifaBridge::BridgeTaskEntry(void* arg) {
    static_cast<SmartRukifaBridge*>(arg)->BridgeTaskLoop();
    vTaskDelete(nullptr);
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
    std::string url = SMARTRUKIFA_BRIDGE_BASE_URL;
    ESP_LOGI(TAG, "Connecting to %s", url.c_str());

    if (!ws_->Connect(url.c_str())) {
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

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddNumberToObject(root, "volume", vol);
    cJSON_AddBoolToObject(root, "intercom", intercom_active_.load());
    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ws_->Send(std::string(json_str));
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
}

void SmartRukifaBridge::SetIntercomActive(bool active) {
    bool prev = intercom_active_.exchange(active);
    if (prev == active) {
        return;
    }

    auto& app = Application::GetInstance();
    app.Schedule([active]() {
        auto& app_ref = Application::GetInstance();
        auto display = Board::GetInstance().GetDisplay();
        if (active) {
            app_ref.GetAudioService().EnableWakeWordDetection(false);
            app_ref.PlaySound(Lang::Sounds::OGG_POPUP);
            if (display) {
                display->ShowNotification("Intercom Ayah Aktif", 5000);
                display->SetChatMessage("system", "Intercom Terhubung");
            }
        } else {
            app_ref.GetAudioService().EnableWakeWordDetection(true);
            if (display) {
                display->ShowNotification("Intercom Selesai", 3000);
                display->SetChatMessage("system", "");
            }
        }
    });
}

void SmartRukifaBridge::HandleBinaryAudio(const char* data, size_t len) {
    if (!data || len < 2) {
        return;
    }
    // Incoming 16kHz 16-bit mono PCM from parent's Walkie-Talkie PTT
    size_t sample_count = len / sizeof(int16_t);
    std::vector<int16_t> samples(sample_count);
    memcpy(samples.data(), data, sample_count * sizeof(int16_t));

    auto& app = Application::GetInstance();
    app.Schedule([samples = std::move(samples)]() {
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
                        std::string msg = "Volume: " + std::to_string(vol) + "%";
                        display->ShowNotification(msg.c_str(), 2500);
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
                    if (is_tts) {
                        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                    }
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
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
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
    std::vector<int16_t> pcm_buf;

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
        if ((now - last_hb) >= pdMS_TO_TICKS(20000)) {
            SendHeartbeat();
            last_hb = now;
        }

        if (intercom_active_.load()) {
            // Stream 20ms (320 samples @ 16kHz) PCM audio from INMP441 to Bridge
            auto& audio = Application::GetInstance().GetAudioService();
            if (audio.ReadAudioData(pcm_buf, 16000, 320) && !pcm_buf.empty()) {
                if (ws_ && ws_->IsConnected()) {
                    ws_->Send(reinterpret_cast<const char*>(pcm_buf.data()),
                              pcm_buf.size() * sizeof(int16_t), true);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}
