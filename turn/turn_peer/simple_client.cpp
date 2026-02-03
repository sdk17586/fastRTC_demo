/**
 * @file simple_client.cpp
 * @brief 간단한 WebRTC 클라이언트 사용 예제
 * 
 * 이 예제는 WebRTCClient 클래스를 사용하여 카메라 스트림을 서버로 전송하는 방법을 보여줍니다.
 * 기존의 복잡한 GStreamer API 호출 없이 매우 간단하게 사용할 수 있습니다.
 */

#include "webrtc_client.h"
#include <gst/gst.h>
#include <iostream>
#include <signal.h>
#include <cstdlib>

static WebRTCClient *g_client = nullptr;

void signal_handler(int /*signal*/) {
    if (g_client) {
        std::cout << "\n종료 중...\n";
        g_client->stop();
    }
    exit(0);
}

int main(int argc, char **argv) {
    // GStreamer 초기화
    gst_init(&argc, &argv);

    // 서버 URL 설정 (기본값: http://localhost:8000, turn_http 프록시)
    std::string server_url = argc > 1 ? argv[1] : "http://localhost:8000";
    std::string turn_server = argc > 2 ? argv[2] : "";
    std::string stun_server = argc > 3 ? argv[3] : "";

    // 카메라 설정 (선택사항)
    WebRTCClient::CameraConfig camera_config;
    camera_config.device = "/dev/video0";
    camera_config.width = 640;
    camera_config.height = 480;
    camera_config.framerate = 30;
    camera_config.codec = "vp8";  // 또는 "h264"

    // TURN 서버 하드코딩 설정
    const std::string kTurnHost = "127.0.0.1";
    const int kTurnPort = 3478;
    const std::string kTurnUsername = "nsm";
    const std::string kTurnPassword = "nsm1234";
    const std::string kTurnScheme = "turn"; // "turn" or "turns"

    if (turn_server.empty()) {
        turn_server = kTurnScheme + "://" + kTurnUsername + ":" + kTurnPassword +
                      "@" + kTurnHost + ":" + std::to_string(kTurnPort);
    }

    // ICE 서버 설정
    WebRTCClient::IceConfig ice_config;
    if (!turn_server.empty()) {
        ice_config.turn_server = turn_server;
        if (stun_server.empty()) {
            ice_config.stun_server.clear();
        }
    }
    if (!stun_server.empty()) {
        ice_config.stun_server = stun_server;
    }
    ice_config.force_relay = true;

    // WebRTC 클라이언트 생성
    WebRTCClient client(server_url, camera_config, ice_config);

    // 전역 변수 설정 (시그널 핸들러용)
    g_client = &client;

    // 시그널 핸들러 등록
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 연결 상태 콜백 설정
    client.setStateCallback([](WebRTCClient::ConnectionState state) {
        switch (state) {
            case WebRTCClient::ConnectionState::DISCONNECTED:
                std::cout << "상태: 연결 끊김\n";
                break;
            case WebRTCClient::ConnectionState::CONNECTING:
                std::cout << "상태: 연결 중...\n";
                break;
            case WebRTCClient::ConnectionState::CONNECTED:
                std::cout << "상태: 연결됨 ✓\n";
                break;
            case WebRTCClient::ConnectionState::FAILED:
                std::cout << "상태: 연결 실패 ✗\n";
                break;
        }
    });

    // 에러 콜백 설정
    client.setErrorCallback([](const std::string& error) {
        std::cerr << "에러: " << error << "\n";
    });

    // 연결 시작
    std::cout << "WebRTC 클라이언트 시작 중...\n";
    std::cout << "서버: " << server_url << "\n";
    if (!ice_config.turn_server.empty()) {
        std::cout << "TURN: " << kTurnScheme << "://" << kTurnHost << ":" << kTurnPort << "\n";
    }
    if (ice_config.force_relay) {
        std::cout << "ICE 정책: relay-only\n";
    }
    if (!ice_config.stun_server.empty()) {
        std::cout << "STUN: " << ice_config.stun_server << "\n";
    }
    std::cout << "카메라: " << camera_config.device << "\n";
    std::cout << "해상도: " << camera_config.width << "x" << camera_config.height 
              << " @ " << camera_config.framerate << "fps\n";
    std::cout << "코덱: " << camera_config.codec << "\n";
    std::cout << "\n종료하려면 Ctrl+C를 누르세요.\n\n";

    if (!client.start()) {
        std::cerr << "클라이언트 시작 실패!\n";
        return 1;
    }

    // 메인 루프 실행 (블로킹)
    client.run();

    return 0;
}
