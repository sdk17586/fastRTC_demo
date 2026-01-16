import asyncio
import cv2
import numpy as np
from aiortc import RTCPeerConnection, RTCSessionDescription, VideoStreamTrack
from aiortc.contrib.media import MediaPlayer
import aiohttp
import json
import uuid
import time

class CameraVideoStreamTrack(VideoStreamTrack):
    """
    카메라에서 영상을 캡처하여 WebRTC로 전송하는 VideoStreamTrack
    """
    def __init__(self):
        super().__init__()
        self.camera = cv2.VideoCapture(0)
        
        if not self.camera.isOpened():
            raise RuntimeError("카메라를 열 수 없습니다!")
        
        # 카메라 해상도 설정
        self.camera.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self.camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        self.camera.set(cv2.CAP_PROP_FPS, 30)
        
        # 프레임 전송 통계
        self.frame_count = 0
        self.last_log_time = time.time()
        
        print("카메라가 성공적으로 열렸습니다.")
    
    async def recv(self):
        """
        카메라에서 프레임을 읽어서 WebRTC 형식으로 반환
        """
        # 타임스탬프 가져오기 (await 필요)
        pts, time_base = await self.next_timestamp()
        
        ret, frame = self.camera.read()
        
        if not ret:
            # 프레임을 읽지 못한 경우 검은 화면 반환
            frame = np.zeros((480, 640, 3), dtype=np.uint8)
        
        # 프레임 전송 통계
        self.frame_count += 1
        current_time = time.time()
        
        # 첫 프레임 전송 시 즉시 로그 출력
        if self.frame_count == 1:
            print(f"[클라이언트] ✅ 첫 프레임 전송! 프레임 크기: {frame.shape}")
            self.last_log_time = current_time
        
        # 1초마다 로그 출력
        if current_time - self.last_log_time >= 1.0:
            fps = self.frame_count / (current_time - self.last_log_time)
            print(f"[클라이언트] 프레임 전송 중... 총 {self.frame_count}개 프레임 전송, FPS: {fps:.2f}")
            self.frame_count = 0
            self.last_log_time = current_time
        
        # 클라이언트 송신 확인 메시지 추가
        cv2.putText(frame, "CLIENT SENDING", (50, 50), 
                    cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
        
        # BGR을 RGB로 변환 (WebRTC는 RGB를 사용)
        frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        
        # VideoFrame 형식으로 변환
        from av import VideoFrame
        video_frame = VideoFrame.from_ndarray(frame_rgb, format="rgb24")
        video_frame.pts = pts
        video_frame.time_base = time_base
        
        return video_frame
    
    def stop(self):
        """
        카메라 리소스 해제
        """
        if self.camera is not None:
            self.camera.release()
        super().stop()

async def connect_to_server(server_url="http://localhost:7860"):
    """
    FastRTC 서버에 WebRTC 연결을 설정합니다.
    
    Args:
        server_url: 서버 URL
    """
    # RTCPeerConnection 생성
    pc = RTCPeerConnection()
    # 서버가 datachannel을 기다리므로 먼저 생성
    pc.createDataChannel("client-data")
    
    # 카메라 비디오 트랙 추가
    video_track = CameraVideoStreamTrack()
    pc.addTrack(video_track)
    
    # Offer 생성
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    
    print(f"서버에 연결 중: {server_url}")
    
    # WebRTC 세션 ID 생성
    webrtc_id = str(uuid.uuid4())
    print(f"WebRTC ID: {webrtc_id}")
    
    # 서버에 offer 전송
    async with aiohttp.ClientSession() as session:
        offer_data = {
            "sdp": pc.localDescription.sdp,
            "type": pc.localDescription.type,
            "webrtc_id": webrtc_id
        }
        
        print(f"Offer 전송 중... SDP 길이: {len(pc.localDescription.sdp)}")
        
        async with session.post(
            f"{server_url}/webrtc/offer",
            json=offer_data,
            headers={"Content-Type": "application/json"}
        ) as resp:
            if resp.status != 200:
                error_text = await resp.text()
                print(f"오류: 서버 연결 실패 (상태 코드: {resp.status})")
                print(f"서버 응답: {error_text}")
                return
            
            answer_data = await resp.json()
            print(f"Answer 수신: type={answer_data.get('type')}")
            
            answer = RTCSessionDescription(
                sdp=answer_data["sdp"],
                type=answer_data["type"]
            )
            await pc.setRemoteDescription(answer)
            print("서버와 연결되었습니다!")
        
        # ICE 후보 처리
        @pc.on("icecandidate")
        async def on_icecandidate(candidate):
            if candidate:
                try:
                    candidate_data = {
                        "candidate": {
                            "candidate": candidate.candidate,
                            "sdpMid": candidate.sdpMid,
                            "sdpMLineIndex": candidate.sdpMLineIndex,
                        },
                        "webrtc_id": webrtc_id
                    }
                    async with session.post(
                        f"{server_url}/webrtc/ice",
                        json=candidate_data,
                        headers={"Content-Type": "application/json"}
                    ) as ice_resp:
                        if ice_resp.status != 200:
                            print(f"ICE candidate 전송 실패: {ice_resp.status}")
                except Exception as e:
                    print(f"ICE candidate 처리 오류: {e}")
        
        # 연결 상태 모니터링
        @pc.on("connectionstatechange")
        async def on_connectionstatechange():
            print(f"연결 상태: {pc.connectionState}")
            if pc.connectionState in ["failed", "closed"]:
                await pc.close()
                video_track.stop()
        
        # 연결 유지
        try:
            print("영상 전송 중... (Ctrl+C로 종료)")
            while True:
                await asyncio.sleep(1)
        except KeyboardInterrupt:
            print("\n연결을 종료합니다...")
        finally:
            await pc.close()
            video_track.stop()
            print("카메라가 해제되었습니다.")

if __name__ == "__main__":
    print("=" * 50)
    print("WebRTC 클라이언트가 시작되었습니다!")
    print("=" * 50)
    
    try:
        asyncio.run(connect_to_server())
    except KeyboardInterrupt:
        print("\n클라이언트를 종료합니다...")
    except Exception as e:
        print(f"오류 발생: {e}")
