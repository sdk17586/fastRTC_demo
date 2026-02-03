#pragma once

#include <string>
#include <functional>
#include <memory>

class WebRTCClient {
public:
    struct CameraConfig {
        std::string device = "/dev/video0";    // 카메라 디바이스 경로
        int width = 640;                       // 비디오 너비
        int height = 480;                      // 비디오 높이
        int framerate = 30;                    // 프레임레이트
        std::string codec = "vp8";             // 비디오 코덱 ("vp8" 또는 "h264")
    };

    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    using StateCallback = std::function<void(ConnectionState state)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    explicit WebRTCClient(const std::string& server_url);

    WebRTCClient(const std::string& server_url, 
                 const CameraConfig& camera_config);
    ~WebRTCClient();

    WebRTCClient(const WebRTCClient&) = delete;
    WebRTCClient& operator=(const WebRTCClient&) = delete;
    WebRTCClient(WebRTCClient&&) = delete;
    WebRTCClient& operator=(WebRTCClient&&) = delete;

    bool start();

    void stop();
    void run();
    void runAsync();
    void setStateCallback(StateCallback callback);
    void setErrorCallback(ErrorCallback callback);
    ConnectionState getState() const;
    void setServerUrl(const std::string& url);
    void setCameraConfig(const CameraConfig& config);

private:
    class Impl;  
    std::unique_ptr<Impl> pImpl;
};

