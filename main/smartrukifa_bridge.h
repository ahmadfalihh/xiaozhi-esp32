#ifndef _SMARTRUKIFA_BRIDGE_H_
#define _SMARTRUKIFA_BRIDGE_H_

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>
#include <string>

class SmartRukifaBridge {
public:
    static SmartRukifaBridge& GetInstance() {
        static SmartRukifaBridge instance;
        return instance;
    }

    void Start();
    bool IsConnected() const { return connected_.load(); }
    bool IsIntercomActive() const { return intercom_active_.load(); }

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

    std::unique_ptr<WebSocket> ws_;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> intercom_active_{false};
};

#endif  // _SMARTRUKIFA_BRIDGE_H_
