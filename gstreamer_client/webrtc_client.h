#pragma once

#include <string>
#include <functional>
#include <memory>

/**
 * @brief 고수준 WebRTC 클라이언트 클래스
 * 
 * GStreamer WebRTC API의 복잡성을 추상화하여 간단한 인터페이스를 제공합니다.
 * 사용자는 카메라 설정과 서버 URL만 지정하면 자동으로 WebRTC 연결을 처리합니다.
 */
class WebRTCClient {
public:
    /**
     * @brief 카메라 설정 구조체
     */
    struct CameraConfig {
        std::string device = "/dev/video0";    // 카메라 디바이스 경로
        int width = 640;                       // 비디오 너비
        int height = 480;                      // 비디오 높이
        int framerate = 30;                    // 프레임레이트
        std::string codec = "vp8";             // 비디오 코덱 ("vp8" 또는 "h264")
    };

    /**
     * @brief 연결 상태 콜백 타입
     */
    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    using StateCallback = std::function<void(ConnectionState state)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    /**
     * @brief 생성자 (기본 카메라 설정 사용)
     * @param server_url 서버 URL (예: "http://localhost:7860")
     */
    explicit WebRTCClient(const std::string& server_url);

    /**
     * @brief 생성자 (카메라 설정 지정)
     * @param server_url 서버 URL (예: "http://localhost:7860")
     * @param camera_config 카메라 설정
     */
    WebRTCClient(const std::string& server_url, 
                 const CameraConfig& camera_config);

    /**
     * @brief 소멸자 - 자동으로 리소스 정리
     */
    ~WebRTCClient();

    // 복사 및 이동 방지
    WebRTCClient(const WebRTCClient&) = delete;
    WebRTCClient& operator=(const WebRTCClient&) = delete;
    WebRTCClient(WebRTCClient&&) = delete;
    WebRTCClient& operator=(WebRTCClient&&) = delete;

    /**
     * @brief WebRTC 연결 시작
     * @return 성공 여부
     */
    bool start();

    /**
     * @brief WebRTC 연결 종료
     */
    void stop();

    /**
     * @brief 메인 루프 실행 (블로킹)
     * 연결이 시작되면 이 함수가 연결이 종료될 때까지 실행됩니다.
     */
    void run();

    /**
     * @brief 비블로킹 실행
     * 별도 스레드에서 run()을 호출할 수 있습니다.
     */
    void runAsync();

    /**
     * @brief 연결 상태 콜백 설정
     * @param callback 상태 변경 시 호출될 콜백 함수
     */
    void setStateCallback(StateCallback callback);

    /**
     * @brief 에러 콜백 설정
     * @param callback 에러 발생 시 호출될 콜백 함수
     */
    void setErrorCallback(ErrorCallback callback);

    /**
     * @brief 현재 연결 상태 가져오기
     */
    ConnectionState getState() const;

    /**
     * @brief 서버 URL 변경
     */
    void setServerUrl(const std::string& url);

    /**
     * @brief 카메라 설정 변경 (연결 전에만 호출 가능)
     */
    void setCameraConfig(const CameraConfig& config);

private:
    class Impl;  // PIMPL 패턴으로 구현 세부사항 숨김
    std::unique_ptr<Impl> pImpl;
};

