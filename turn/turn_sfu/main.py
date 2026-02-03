from __future__ import annotations

import logging
import time

import uvicorn

from signaling_server import SignalingServer
from webrtc_service import WebRTCService

logging.basicConfig(level=logging.INFO)
# RTP 패킷 로그를 줄이기 위해 INFO 레벨로 설정
logging.getLogger("aiortc").setLevel(logging.INFO)
logging.getLogger("aioice").setLevel(logging.INFO)
# RTP 패킷 관련 로거는 WARNING 레벨로 설정하여 출력 억제
logging.getLogger("aiortc.rtcrtpsender").setLevel(logging.WARNING)
logging.getLogger("aiortc.rtcrtpreceiver").setLevel(logging.WARNING)

class _RtpCarriageHandler(logging.Handler):
    def __init__(self) -> None:
        super().__init__(logging.DEBUG)
        self._last_ts = time.time()
        self._count = 0
        self._last_msg = ""

    def emit(self, record: logging.LogRecord) -> None:
        msg = record.getMessage()
        if not msg:
            return
        self._count += 1
        self._last_msg = msg
        now = time.time()
        if now - self._last_ts >= 1.0:
            print(
                f"\r[server][rtp] packets:{self._count} last:{self._last_msg}    ",
                end="",
                flush=True,
            )
            self._count = 0
            self._last_ts = now


_rtp_logger = logging.getLogger("aiortc.rtcrtpreceiver")
_rtp_logger.setLevel(logging.DEBUG)
_rtp_logger.addHandler(_RtpCarriageHandler())
_rtp_logger.propagate = False


def main() -> None:
    # TURN 서버 설정 (하드코딩)
    turn_urls = ["turn:127.0.0.1:3478"]
    turn_username = "nsm"
    turn_password = "nsm1234"

    webrtc = WebRTCService(
        turn_urls=turn_urls,
        turn_username=turn_username,
        turn_password=turn_password,
    )
    server = SignalingServer(webrtc)
    app = server.create_app()
    print("=" * 50)
    print("WebRTC 서버가 시작되었습니다!")
    print("서버 주소: http://localhost:7860")
    print("클라이언트가 연결을 기다리는 중...")
    print("=" * 50)
    uvicorn.run(app, host="0.0.0.0", port=7860, access_log=False)


if __name__ == "__main__":
    main()
