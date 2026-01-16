# FastRTC WebRTC 카메라 스트리밍

이 프로젝트는 FastRTC를 사용하여 클라이언트에서 서버로 카메라 영상을 WebRTC로 전송하는 Python 애플리케이션입니다.

## 설치

```bash
cd /root/fastrtc
pip install -r requirements.txt
```

## 사용 방법

### 1. 서버 실행

터미널 1에서 서버를 실행합니다:

```bash
python server.py
```

서버는 `http://localhost:7860`에서 실행됩니다.

### 2. 클라이언트 실행

터미널 2에서 클라이언트를 실행합니다:

```bash
python client.py
```

클라이언트가 카메라를 열고 서버에 연결하여 영상을 전송합니다.

## 파일 설명

- `server.py`: FastRTC 서버 - 클라이언트로부터 영상을 수신하고 처리합니다
- `client.py`: WebRTC 클라이언트 - 카메라 영상을 캡처하여 서버로 전송합니다
- `requirements.txt`: 필요한 Python 패키지 목록

## 주의사항

1. 카메라가 시스템에 연결되어 있어야 합니다
2. 서버를 먼저 실행한 후 클라이언트를 실행해야 합니다
3. 방화벽 설정에 따라 STUN/TURN 서버가 필요할 수 있습니다

## 종료

`Ctrl+C`를 눌러 클라이언트와 서버를 종료할 수 있습니다.

