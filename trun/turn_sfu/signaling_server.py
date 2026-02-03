from __future__ import annotations

import asyncio
import contextlib
import time
from collections.abc import AsyncGenerator, Callable
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import StreamingResponse

from webrtc_service import WebRTCService


class SignalingServer:
    def __init__(self, webrtc: WebRTCService) -> None:
        self.webrtc = webrtc
        self._ice_queues: dict[str, asyncio.Queue[dict]] = {}
        self._ice_complete: set[str] = set()
        self._ice_handlers: set[str] = set()
        self._ice_seen: dict[str, set[str]] = {}
        self._answer_sent: set[str] = set()
        self._pre_answer_ice: dict[str, list[dict]] = {}
        self._ice_poll_count = 0
        self._ice_poll_last_ts = time.time()

    def _register_pc_ice_handler(self, webrtc_id: str, pc) -> None:
        if webrtc_id in self._ice_handlers:
            return
        self._ice_handlers.add(webrtc_id)
        self._ice_queues.setdefault(webrtc_id, asyncio.Queue())
        self._ice_seen.setdefault(webrtc_id, set())
        self._pre_answer_ice.setdefault(webrtc_id, [])

        def mark_complete() -> None:
            if webrtc_id in self._ice_complete:
                return
            self._ice_complete.add(webrtc_id)
            asyncio.create_task(
                self._ice_queues[webrtc_id].put({"type": "ice-complete"})
            )

        def queue_candidate(candidate_str: str, sdp_mid: str, sdp_mline_index: int) -> None:
            key = f"{sdp_mline_index}:{candidate_str}"
            if key in self._ice_seen[webrtc_id]:
                return
            self._ice_seen[webrtc_id].add(key)
            payload = {
                "type": "ice-candidate",
                "candidate": {
                    "candidate": candidate_str,
                    "sdpMid": sdp_mid,
                    "sdpMLineIndex": sdp_mline_index,
                },
            }
            if webrtc_id not in self._answer_sent:
                self._pre_answer_ice[webrtc_id].append(payload)
                return
            self._log_ice_send(webrtc_id, sdp_mid, sdp_mline_index, candidate_str)
            asyncio.create_task(self._ice_queues[webrtc_id].put(payload))

        @pc.on("icecandidate")
        def on_icecandidate(candidate) -> None:
            if candidate is None:
                mark_complete()
                return
            sdp_mid = getattr(candidate, "sdpMid", "") or ""
            sdp_mline_index = getattr(candidate, "sdpMLineIndex", 0) or 0
            candidate_str = getattr(candidate, "candidate", "") or ""
            if candidate_str:
                queue_candidate(candidate_str, sdp_mid, int(sdp_mline_index))

        @pc.on("iceconnectionstatechange")
        def on_iceconnectionstatechange() -> None:
            if pc.iceConnectionState in ("connected", "completed"):
                mark_complete()

        local_desc = pc.localDescription
        if local_desc and local_desc.sdp:
            self._seed_ice_from_sdp(webrtc_id, local_desc.sdp, queue_candidate)

    def _drain_ice_queue(self, webrtc_id: str) -> tuple[list[dict], bool]:
        queue = self._ice_queues.get(webrtc_id)
        if not queue:
            return [], webrtc_id in self._ice_complete
        drained: list[dict] = []
        while True:
            try:
                drained.append(queue.get_nowait())
            except asyncio.QueueEmpty:
                break
        complete = webrtc_id in self._ice_complete or any(
            item.get("type") == "ice-complete" for item in drained
        )
        return drained, complete

    def _flush_pre_answer_ice(self, webrtc_id: str) -> None:
        pending = self._pre_answer_ice.get(webrtc_id)
        if not pending:
            return
        for item in pending:
            candidate = item.get("candidate", {})
            candidate_str = candidate.get("candidate", "")
            sdp_mid = candidate.get("sdpMid", "")
            sdp_mline_index = candidate.get("sdpMLineIndex", 0)
            if candidate_str:
                self._log_ice_send(webrtc_id, sdp_mid, sdp_mline_index, candidate_str)
            asyncio.create_task(self._ice_queues[webrtc_id].put(item))
        pending.clear()

    def _log_ice_send(
        self, webrtc_id: str, sdp_mid: str, sdp_mline_index: int, candidate_str: str
    ) -> None:
        print("\n========================================")
        print("📤 ICE CANDIDATE 송신 (서버 -> 클라이언트)")
        print("========================================\n")
        print("[server] Sending ICE candidate to client")
        print(f"[server] WebRTC ID: {webrtc_id}")
        print(f"[server] SDP MID: {sdp_mid}")
        print(f"[server] SDP MLine Index: {sdp_mline_index}")
        print(f"[server] Candidate String: {candidate_str}")
        print("[server] ICE Candidate Details:")
        self._parse_ice_candidate_details(candidate_str)

    def _seed_ice_from_sdp(
        self,
        webrtc_id: str,
        sdp: str,
        queue_candidate: Callable[[str, str, int], None],
    ) -> None:
        if not sdp:
            return
        mline_index = -1
        current_mid = ""
        for raw_line in sdp.splitlines():
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("m="):
                mline_index += 1
                current_mid = ""
                continue
            if line.startswith("a=mid:"):
                current_mid = line[6:]
                continue
            if line.startswith("a=candidate:"):
                candidate_str = line[2:]
                queue_candidate(
                    candidate_str,
                    current_mid or "0",
                    mline_index if mline_index >= 0 else 0,
                )

    async def handle_webrtc_ice(self, request: Request):
        body = await request.json()
        body.setdefault("type", "ice-candidate")

        # ICE 수신 로그 출력
        print("\n========================================")
        print("📥 ICE CANDIDATE 수신 (클라이언트 -> 서버)")
        print("========================================\n")

        candidate_obj = body.get("candidate", {})
        candidate_str = (
            candidate_obj.get("candidate", "") if isinstance(candidate_obj, dict) else ""
        )
        sdp_mid = (
            candidate_obj.get("sdpMid", "") if isinstance(candidate_obj, dict) else ""
        )
        sdp_mline_index = (
            candidate_obj.get("sdpMLineIndex", -1)
            if isinstance(candidate_obj, dict)
            else -1
        )
        webrtc_id = body.get("webrtc_id", "")

        print(f"[server] Received ICE candidate from client")
        print(f"[server] WebRTC ID: {webrtc_id}")
        print(f"[server] SDP MID: {sdp_mid}")
        print(f"[server] SDP MLine Index: {sdp_mline_index}")
        print(f"[server] Candidate String: {candidate_str}")
        print(f"[server] ICE Candidate Details:")
        self._parse_ice_candidate_details(candidate_str)

        self.webrtc.normalize_candidate(body)
        if not webrtc_id:
            print("❌ ICE CANDIDATE 처리 실패 (webrtc_id 없음)\n")
            return {"status": "failed", "meta": {"error": "missing_webrtc_id"}}

        result = await self.webrtc.stream.handle_offer(
            body, set_outputs=self.webrtc.stream.set_additional_outputs(webrtc_id)
        )

        print("✅ ICE CANDIDATE 처리 완료\n")
        return result

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

        result = await self.webrtc.stream.handle_offer(
            body, set_outputs=self.webrtc.stream.set_additional_outputs(webrtc_id)
        )

        pc = self.webrtc.stream.pcs.get(webrtc_id)
        if pc:
            self._register_pc_ice_handler(webrtc_id, pc)
            result = {
                "sdp": self._strip_ice_from_sdp(pc.localDescription.sdp),
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
            self._answer_sent.add(webrtc_id)
            self._flush_pre_answer_ice(webrtc_id)
        else:
            print("\n========================================")
            print("❌ ANSWER 생성 실패 (PeerConnection 없음)")
            print("========================================\n")

        return result

    async def handle_webrtc_ice_poll(self, request: Request):
        self._ice_poll_count += 1
        now = time.time()
        if now - self._ice_poll_last_ts >= 1.0:
            print(
                f"\r[sfu] ICE poll req/s: {self._ice_poll_count}    ",
                end="",
                flush=True,
            )
            self._ice_poll_count = 0
            self._ice_poll_last_ts = now
        webrtc_id = request.query_params.get("webrtc_id")
        if not webrtc_id:
            return {"status": "failed", "meta": {"error": "missing_webrtc_id"}}
        drained, complete = self._drain_ice_queue(webrtc_id)
        candidates = [
            item["candidate"]
            for item in drained
            if item.get("type") == "ice-candidate"
        ]
        return {"status": "ok", "candidates": candidates, "complete": complete}

    def handle_root(self) -> dict[str, str]:
        return {
            "status": "running",
            "message": "FastRTC WebRTC 서버가 실행 중입니다",
            "webrtc_endpoint": "/webrtc/offer",
            "mjpeg_preview": "/video",
        }

    def handle_video_feed(self) -> StreamingResponse:
        return StreamingResponse(
            self.webrtc.mjpeg_generator(),
            media_type="multipart/x-mixed-replace; boundary=frame",
        )

    def _register_routes(self, app: FastAPI) -> None:
        app.post("/webrtc/offer")(self.handle_webrtc_offer)
        app.post("/webrtc/ice")(self.handle_webrtc_ice)
        app.get("/webrtc/ice")(self.handle_webrtc_ice_poll)
        app.get("/")(self.handle_root)
        app.get("/video")(self.handle_video_feed)

    def _parse_ice_candidate_details(self, candidate_str: str) -> None:
        """Parse and print detailed ICE candidate information"""
        if not candidate_str:
            return

        # ICE candidate 형식: candidate:<foundation> <component-id> <transport> <priority> <ip> <port> typ <type> [options...]
        if ":" not in candidate_str:
            print(f"    Raw candidate: {candidate_str}")
            return

        rest = candidate_str.split(":", 1)[1]
        parts = rest.split()

        if len(parts) >= 7:
            print(f"    Foundation: {parts[0]}")
            print(f"    Component ID: {parts[1]}")
            print(f"    Transport: {parts[2]}")
            print(f"    Priority: {parts[3]}")
            print(f"    IP Address: {parts[4]}")
            print(f"    Port: {parts[5]}")

            # typ 필드 찾기
            for i in range(6, len(parts)):
                if parts[i] == "typ" and i + 1 < len(parts):
                    print(f"    Type: {parts[i + 1]}")
                    break

            # 추가 옵션 파싱
            for i in range(6, len(parts)):
                if parts[i] == "raddr" and i + 1 < len(parts):
                    print(f"    Remote Address: {parts[i + 1]}")
                elif parts[i] == "rport" and i + 1 < len(parts):
                    print(f"    Remote Port: {parts[i + 1]}")
                elif parts[i] == "generation" and i + 1 < len(parts):
                    print(f"    Generation: {parts[i + 1]}")
                elif parts[i] == "ufrag" and i + 1 < len(parts):
                    print(f"    ICE Ufrag: {parts[i + 1]}")
                elif parts[i] == "network-id" and i + 1 < len(parts):
                    print(f"    Network ID: {parts[i + 1]}")
                elif parts[i] == "network-cost" and i + 1 < len(parts):
                    print(f"    Network Cost: {parts[i + 1]}")
                elif parts[i] == "tcptype" and i + 1 < len(parts):
                    print(f"    TCP Type: {parts[i + 1]}")
        else:
            print(f"    Raw candidate: {rest}")

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
                    print(
                        "Origin: username={}, sess-id={}, sess-version={}, "
                        "nettype={}, addrtype={}, address={}".format(
                            parts[0],
                            parts[1],
                            parts[2],
                            parts[3],
                            parts[4],
                            parts[5],
                        )
                    )
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
                    print(
                        f"Connection: nettype={parts[0]}, addrtype={parts[1]}, "
                        f"address={parts[2]}"
                    )
            elif line.startswith("t="):
                parts = line[2:].split()
                if len(parts) >= 2:
                    print(f"Timing: start={parts[0]}, stop={parts[1]}")
            elif line.startswith("a="):
                attr = line[2:]
                if ":" in attr:
                    key, value = attr.split(":", 1)
                    if key in [
                        "fingerprint",
                        "setup",
                        "ice-ufrag",
                        "ice-pwd",
                        "ice-options",
                        "rtcp-mux",
                    ]:
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
                    print(
                        f"Media: type={media_type}, port={port}, "
                        f"protocol={protocol}, formats=[{formats}]"
                    )
                    current_media = media_type

        print(f"--- End of {title} SDP Details ---\n")

    def _strip_ice_from_sdp(self, sdp_text: str) -> str:
        if not sdp_text:
            return sdp_text
        lines = []
        for raw_line in sdp_text.splitlines():
            line = raw_line.strip()
            if line.startswith("a=candidate:") or line.startswith("a=end-of-candidates"):
                continue
            lines.append(raw_line)
        return "\n".join(lines)

    def create_app(self) -> FastAPI:
        @asynccontextmanager
        async def lifespan(_app: FastAPI) -> AsyncGenerator[None, None]:
            task = asyncio.create_task(self.webrtc.ensure_default_input())
            try:
                yield
            finally:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await task

        app = FastAPI(lifespan=lifespan)
        self.webrtc.stream.mount(app, path="/_internal")
        self._register_routes(app)
        return app
