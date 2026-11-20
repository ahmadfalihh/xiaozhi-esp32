#ifndef _SMARTRUKIFA_BRIDGE_H_
#define _SMARTRUKIFA_BRIDGE_H_

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class SmartRukifaBridge {
public:
    static SmartRukifaBridge& GetInstance() {
        static SmartRukifaBridge instance;
        return instance;
    }

    void Start();
    bool IsConnected() const { return connected_.load(); }
    bool IsIntercomActive() const { return intercom_active_.load(); }

    // Called safely from AudioService::ReadAudioData (16kHz mono PCM)
    void FeedMicPcm(const std::vector<int16_t>& pcm);

private:
    SmartRukifaBridge() = default;
    ~SmartRukifaBridge() = default;

    static void BridgeTaskEntry(void* arg);
    void BridgeTaskLoop();
    bool Connect();
    void HandleTextCommand(const char* data, size_t len);
    void HandleBinaryAudio(const char* data, size_t len);
    void SendHeartbeat();
    void SetIntercomActive(bool active);

    static constexpr size_t kChunkSamples = 480;     // 30ms @ 16kHz = 960 bytes
    static constexpr size_t kRingCapacity = 1920;    // 120ms fixed static ring (no heap alloc)

    std::unique_ptr<WebSocket> ws_;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> intercom_active_{false};

    std::mutex pcm_mutex_;
    int16_t ring_buf_[kRingCapacity];
    size_t ring_head_ = 0;
    size_t ring_count_ = 0;
};

#endif  // _SMARTRUKIFA_BRIDGE_H_
