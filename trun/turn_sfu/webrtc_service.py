from __future__ import annotations

import asyncio
import os
import threading
import time
from collections.abc import Generator

import cv2
import numpy as np
from fastrtc import Stream
from fastrtc.tracks import VideoCallback

# Allow video processing even when no data channel is negotiated.
_orig_video_callback_init = VideoCallback.__init__


def _video_callback_init_no_channel(self, *args, **kwargs):
    _orig_video_callback_init(self, *args, **kwargs)
    if self.channel is None:
        self.channel_set.set()


VideoCallback.__init__ = _video_callback_init_no_channel


class WebRTCService:
    MJPEG_SLEEP = 0.03
    NO_FRAME_SLEEP = 0.05
    INPUT_POLL_INTERVAL = 0.1

    def __init__(
        self,
        turn_urls: list[str] | None = None,
        turn_username: str = "",
        turn_password: str = "",
    ) -> None:
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

        self._turn_urls = turn_urls or []
        self._turn_username = turn_username
        self._turn_password = turn_password

        self.stream = self._build_stream()

        self._initialized_webrtc_ids: set[str] = set()

    def _log(self, message: str) -> None:
        print(f"[서버] {message}")

    def _make_handler(self):
        def handler(frame: np.ndarray):
            return self.process_received_frame(frame)

        return handler

    def _build_stream(self) -> Stream:
        ice_servers = self._build_ice_servers()
        server_rtc_configuration = {"iceServers": ice_servers} if ice_servers else None
        try:
            stream = Stream(
                handler=self._handler,
                modality="video",
                mode="send-receive",
                allow_extra_tracks=True,
                server_rtc_configuration=server_rtc_configuration,
            )
            self._log("Stream 객체 생성 완료")
            self._log(f"Stream 객체: {stream}")
            self._log(
                "Stream handler: "
                + (stream.handler if hasattr(stream, "handler") else "N/A")
            )
            if ice_servers:
                self._log(f"TURN 서버 설정됨: {ice_servers}")
            return stream
        except Exception as e:
            self._log(f"❌ Stream 생성 오류: {e}")
            import traceback

            traceback.print_exc()
            raise

    def _build_ice_servers(self) -> list[dict[str, str]]:
        if not self._turn_urls:
            return []
        server = {"urls": self._turn_urls}
        if self._turn_username:
            server["username"] = self._turn_username
        if self._turn_password:
            server["credential"] = self._turn_password
        return [server]

    def _update_latest_frame(self, frame: np.ndarray) -> None:
        with self.latest_frame_lock:
            self.latest_frame = frame.copy()

    def _log_frame_stats(self, frame: np.ndarray, current_time: float) -> None:
        if self.frame_count == 1:
            # 캐리지 리턴으로 같은 줄에 덮어쓰기
            # message = (
            #     f"[서버] ✅ 첫 프레임 수신! 프레임 크기: {frame.shape}, "
            #     f"데이터 타입: {frame.dtype}"
            # )
            # print(f"\r{message:<100}", end="", flush=True)
            self.last_log_time = current_time

        if current_time - self.last_log_time >= 1.0:
            fps = self.frame_count / (current_time - self.last_log_time)
            # 캐리지 리턴으로 같은 줄에 덮어쓰기
            # message = (
            #     f"[서버] 프레임 수신 중... 총 {self.frame_count}개 프레임 수신, "
            #     f"FPS: {fps:.2f}, 크기: {frame.shape}, 타입: {frame.dtype}"
            # )
            # print(f"\r{message:<100}", end="", flush=True)
            self.frame_count = 0
            self.last_log_time = current_time

    def process_received_frame(self, frame: np.ndarray):
        """
        클라이언트로부터 받은 영상 프레임을 처리합니다.
        """
        if frame is None:
            print("\r[서버] ⚠️ 프레임이 None입니다!", flush=True)
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

    async def ensure_default_input(self) -> None:
        while True:
            for webrtc_id, conns in list(self.stream.connections.items()):
                if webrtc_id in self._initialized_webrtc_ids:
                    continue
                if conns:
                    self.stream.set_input(webrtc_id, None)
                    self._initialized_webrtc_ids.add(webrtc_id)
                    self._log(f"기본 입력 초기화 완료: {webrtc_id}")
            await asyncio.sleep(self.INPUT_POLL_INTERVAL)

    def mjpeg_generator(self) -> Generator[bytes, None, None]:
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

    async def wait_for_ice_complete(self, pc, timeout: float = 5.0) -> None:
        if pc.iceGatheringState == "complete":
            return

        done = asyncio.Event()

        @pc.on("icegatheringstatechange")
        async def _():
            if pc.iceGatheringState == "complete":
                done.set()

        try:
            await asyncio.wait_for(done.wait(), timeout)
        except asyncio.TimeoutError:
            self._log("ICE gathering timeout; returning partial SDP")

    def normalize_candidate(self, body: dict) -> None:
        candidate = body.get("candidate")
        if not isinstance(candidate, dict):
            return
        cand_str = candidate.get("candidate")
        if not isinstance(cand_str, str):
            return
        parts = cand_str.split()
        if not parts or not parts[0].startswith("candidate:"):
            return
        if "typ" not in parts:
            return
        if len(parts) >= 3:
            parts[2] = parts[2].lower()
        if len(parts) < 10:
            parts.extend(["generation", "0"])
        candidate["candidate"] = " ".join(parts)
