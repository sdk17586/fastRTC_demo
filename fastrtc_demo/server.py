from __future__ import annotations

import asyncio
import contextlib
import logging
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

logging.basicConfig(level=logging.INFO)
# RTP 패킷 로그를 줄이기 위해 INFO 레벨로 설정
logging.getLogger("aiortc").setLevel(logging.INFO)
logging.getLogger("aioice").setLevel(logging.INFO)
# RTP 패킷 관련 로거는 WARNING 레벨로 설정하여 출력 억제
logging.getLogger("aiortc.rtcrtpsender").setLevel(logging.WARNING)
logging.getLogger("aiortc.rtcrtpreceiver").setLevel(logging.WARNING)


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
            # 캐리지 리턴으로 같은 줄에 덮어쓰기
            message = (
                f"[서버] ✅ 첫 프레임 수신! 프레임 크기: {frame.shape}, "
                f"데이터 타입: {frame.dtype}"
            )
            print(f"\r{message:<100}", end="", flush=True)
            self.last_log_time = current_time

        if current_time - self.last_log_time >= 1.0:
            fps = self.frame_count / (current_time - self.last_log_time)
            # 캐리지 리턴으로 같은 줄에 덮어쓰기
            message = (
                f"[서버] 프레임 수신 중... 총 {self.frame_count}개 프레임 수신, "
                f"FPS: {fps:.2f}, 크기: {frame.shape}, 타입: {frame.dtype}"
            )
            print(f"\r{message:<100}", end="", flush=True)
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
        self._normalize_candidate(body)
        webrtc_id = body.get("webrtc_id")
        if not webrtc_id:
            return {"status": "failed", "meta": {"error": "missing_webrtc_id"}}
        return await self.stream.handle_offer(
            body, set_outputs=self.stream.set_additional_outputs(webrtc_id)
        )

    async def handle_webrtc_offer(self, request: Request):
        body = await request.json()
        webrtc_id = body.get("webrtc_id")
        if not webrtc_id:
            return {"status": "failed", "meta": {"error": "missing_webrtc_id"}}

        # Offer 수신 로그 출력
        print("\n========================================")
        print("📥 OFFER 수신 (클라이언트 -> 서버)")
        print("========================================\n")
        offer_sdp = body.get("sdp", "")
        offer_type = body.get("type", "")
        print(f"[server] Received offer from client")
        print(f"[server] Offer SDP length: {len(offer_sdp)} bytes")
        print(f"[server] Offer type: {offer_type}")
        print(f"[server] WebRTC ID: {webrtc_id}")
        
        # SDP 상세 정보 출력
        self._parse_sdp_details(offer_sdp, "OFFER")
        
        # Full SDP text 출력
        print("[server] Full OFFER SDP:")
        print(offer_sdp)
        print()

        result = await self.stream.handle_offer(
            body, set_outputs=self.stream.set_additional_outputs(webrtc_id)
        )

        pc = self.stream.pcs.get(webrtc_id)
        if pc:
            await self._wait_for_ice_complete(pc)
            result = {
                "sdp": pc.localDescription.sdp,
                "type": pc.localDescription.type,
            }
            
            # Answer 송신 로그 출력
            print("\n========================================")
            print("📤 ANSWER 송신 (서버 -> 클라이언트)")
            print("========================================\n")
            answer_sdp = result.get("sdp", "")
            answer_type = result.get("type", "")
            print(f"[server] Sending answer to client")
            print(f"[server] Answer SDP length: {len(answer_sdp)} bytes")
            print(f"[server] Answer type: {answer_type}")
            
            # SDP 상세 정보 출력
            self._parse_sdp_details(answer_sdp, "ANSWER")
            
            # Full SDP text 출력
            print("[server] Full ANSWER SDP:")
            print(answer_sdp)
            print()
            
            print("✅ ANSWER 전송 완료\n")
        else:
            print("\n========================================")
            print("❌ ANSWER 생성 실패 (PeerConnection 없음)")
            print("========================================\n")
            
        return result

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
        app.post("/webrtc/offer")(self.handle_webrtc_offer)
        app.post("/webrtc/ice")(self.handle_webrtc_ice)
        app.get("/")(self.handle_root)
        app.get("/video")(self.handle_video_feed)

    def _parse_sdp_details(self, sdp_text: str, title: str) -> None:
        """Parse and print detailed SDP information"""
        print(f"\n--- {title} SDP Details ---")
        
        lines = sdp_text.split("\n")
        current_media = None
        
        for line in lines:
            line = line.strip()
            if not line:
                continue
                
            if line.startswith("v="):
                print(f"Version: {line[2:]}")
            elif line.startswith("o="):
                parts = line[2:].split()
                if len(parts) >= 6:
                    print(f"Origin: username={parts[0]}, sess-id={parts[1]}, sess-version={parts[2]}, nettype={parts[3]}, addrtype={parts[4]}, address={parts[5]}")
            elif line.startswith("s="):
                print(f"Session Name: {line[2:]}")
            elif line.startswith("i="):
                print(f"Session Info: {line[2:]}")
            elif line.startswith("u="):
                print(f"URI: {line[2:]}")
            elif line.startswith("e="):
                print(f"Email: {line[2:]}")
            elif line.startswith("p="):
                print(f"Phone: {line[2:]}")
            elif line.startswith("c="):
                parts = line[2:].split()
                if len(parts) >= 3:
                    print(f"Connection: nettype={parts[0]}, addrtype={parts[1]}, address={parts[2]}")
            elif line.startswith("t="):
                parts = line[2:].split()
                if len(parts) >= 2:
                    print(f"Timing: start={parts[0]}, stop={parts[1]}")
            elif line.startswith("a="):
                attr = line[2:]
                if ":" in attr:
                    key, value = attr.split(":", 1)
                    if key in ["fingerprint", "setup", "ice-ufrag", "ice-pwd", "ice-options", "rtcp-mux"]:
                        print(f"  Attribute: {key}={value}")
                    elif key.startswith("rtpmap"):
                        print(f"  RTP Map: {value}")
                    elif key.startswith("fmtp"):
                        print(f"  Format Parameters: {value}")
                    elif key.startswith("ssrc"):
                        print(f"  SSRC: {value}")
                    else:
                        print(f"  Attribute: {attr}")
                else:
                    if attr in ["sendrecv", "sendonly", "recvonly", "inactive"]:
                        print(f"  Direction: {attr}")
                    elif attr.startswith("mid:"):
                        print(f"  Media ID: {attr[4:]}")
                    else:
                        print(f"  Attribute: {attr}")
            elif line.startswith("m="):
                if current_media is not None:
                    print()  # Separate media blocks
                parts = line[2:].split()
                if len(parts) >= 3:
                    media_type = parts[0]
                    port = parts[1]
                    protocol = parts[2]
                    formats = " ".join(parts[3:]) if len(parts) > 3 else "none"
                    print(f"Media: type={media_type}, port={port}, protocol={protocol}, formats=[{formats}]")
                    current_media = media_type
        
        print(f"--- End of {title} SDP Details ---\n")

    def _normalize_candidate(self, body: dict) -> None:
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
        self.stream.mount(app, path="/_internal")
        self._register_routes(app)
        return app

    async def _wait_for_ice_complete(self, pc, timeout: float = 5.0) -> None:
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
