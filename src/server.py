from __future__ import annotations

import asyncio
import contextlib
import threading
import time
from collections.abc import AsyncGenerator, Generator
from contextlib import asynccontextmanager

import cv2
import numpy as np
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import StreamingResponse
from fastrtc import Stream


class FastRTCServer:
    MJPEG_SLEEP = 0.03
    NO_FRAME_SLEEP = 0.05
    INPUT_POLL_INTERVAL = 0.1

    def __init__(self) -> None:
        # 프레임 수신 통계
        self.frame_count = 0
        self.last_log_time = time.time()

        # 브라우저 MJPEG 출력용
        self.latest_frame: np.ndarray | None = None
        self.latest_frame_lock = threading.Lock()

        self._handler = self._make_handler()

        self._log("Stream 객체 생성 중...")
        self._log(f"Handler 함수: {self._handler}")
        self._log(f"Handler 함수 타입: {type(self._handler)}")

        self.stream = self._build_stream()

        self._initialized_webrtc_ids: set[str] = set()

    def _log(self, message: str) -> None:
        print(f"[서버] {message}")

    def _make_handler(self):
        def handler(frame: np.ndarray):
            return self.process_received_frame(frame)

        return handler

    def _build_stream(self) -> Stream:
        try:
            stream = Stream(
                handler=self._handler,
                modality="video",
                mode="send-receive",
                allow_extra_tracks=True,
            )
            self._log("Stream 객체 생성 완료")
            self._log(f"Stream 객체: {stream}")
            self._log(
                "Stream handler: "
                + (stream.handler if hasattr(stream, "handler") else "N/A")
            )
            return stream
        except Exception as e:
            self._log(f"❌ Stream 생성 오류: {e}")
            import traceback

            traceback.print_exc()
            raise

    def _update_latest_frame(self, frame: np.ndarray) -> None:
        with self.latest_frame_lock:
            self.latest_frame = frame.copy()

    def _log_frame_stats(self, frame: np.ndarray, current_time: float) -> None:
        if self.frame_count == 1:
            self._log(
                f"✅ 첫 프레임 수신! 프레임 크기: {frame.shape}, "
                f"데이터 타입: {frame.dtype}"
            )
            self.last_log_time = current_time

        if current_time - self.last_log_time >= 1.0:
            fps = self.frame_count / (current_time - self.last_log_time)
            self._log(
                f"프레임 수신 중... 총 {self.frame_count}개 프레임 수신, "
                f"FPS: {fps:.2f}"
            )
            self._log(f"프레임 크기: {frame.shape}, 데이터 타입: {frame.dtype}")
            self.frame_count = 0
            self.last_log_time = current_time

    def process_received_frame(self, frame: np.ndarray):
        """
        클라이언트로부터 받은 영상 프레임을 처리합니다.
        """
        self._log(
            "🔵 Handler 호출됨! 프레임 크기: "
            f"{frame.shape if frame is not None else 'None'}"
        )

        if frame is None:
            self._log("⚠️ 프레임이 None입니다!")
            return None

        self.frame_count += 1
        current_time = time.time()

        self._log_frame_stats(frame, current_time)

        cv2.putText(
            frame,
            "SERVER RECEIVED: OK",
            (50, 50),
            cv2.FONT_HERSHEY_SIMPLEX,
            1,
            (0, 255, 0),
            2,
        )

        self._update_latest_frame(frame)
        return frame

    async def _ensure_default_input(self) -> None:
        while True:
            for webrtc_id, conns in list(self.stream.connections.items()):
                if webrtc_id in self._initialized_webrtc_ids:
                    continue
                if conns:
                    self.stream.set_input(webrtc_id, None)
                    self._initialized_webrtc_ids.add(webrtc_id)
                    self._log(f"기본 입력 초기화 완료: {webrtc_id}")
            await asyncio.sleep(self.INPUT_POLL_INTERVAL)

    def _mjpeg_generator(self) -> Generator[bytes, None, None]:
        while True:
            with self.latest_frame_lock:
                frame = None if self.latest_frame is None else self.latest_frame.copy()
            if frame is None:
                time.sleep(self.NO_FRAME_SLEEP)
                continue
            ok, buffer = cv2.imencode(".jpg", frame)
            if not ok:
                continue
            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n"
                + buffer.tobytes()
                + b"\r\n"
            )
            time.sleep(self.MJPEG_SLEEP)

    async def handle_webrtc_ice(self, request: Request):
        body = await request.json()
        body.setdefault("type", "ice-candidate")
        webrtc_id = body.get("webrtc_id")
        if not webrtc_id:
            return {"status": "failed", "meta": {"error": "missing_webrtc_id"}}
        return await self.stream.handle_offer(
            body, set_outputs=self.stream.set_additional_outputs(webrtc_id)
        )

    def handle_root(self) -> dict[str, str]:
        return {
            "status": "running",
            "message": "FastRTC WebRTC 서버가 실행 중입니다",
            "webrtc_endpoint": "/webrtc/offer",
            "mjpeg_preview": "/video",
        }

    def handle_video_feed(self) -> StreamingResponse:
        return StreamingResponse(
            self._mjpeg_generator(),
            media_type="multipart/x-mixed-replace; boundary=frame",
        )

    def _register_routes(self, app: FastAPI) -> None:
        app.post("/webrtc/ice")(self.handle_webrtc_ice)
        app.get("/")(self.handle_root)
        app.get("/video")(self.handle_video_feed)

    def create_app(self) -> FastAPI:
        @asynccontextmanager
        async def lifespan(_app: FastAPI) -> AsyncGenerator[None, None]:
            task = asyncio.create_task(self._ensure_default_input())
            try:
                yield
            finally:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await task

        app = FastAPI(lifespan=lifespan)
        self.stream.mount(app)
        self._register_routes(app)
        return app


def main() -> None:
    server = FastRTCServer()
    app = server.create_app()
    print("=" * 50)
    print("WebRTC 서버가 시작되었습니다!")
    print("서버 주소: http://localhost:7860")
    print("클라이언트가 연결을 기다리는 중...")
    print("=" * 50)
    uvicorn.run(app, host="0.0.0.0", port=7860)


if __name__ == "__main__":
    main()
