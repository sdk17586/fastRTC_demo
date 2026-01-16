from fastapi import FastAPI, Request
from fastrtc import Stream
import cv2
import numpy as np
import uvicorn
import time
import asyncio

# 프레임 수신 통계
frame_count = 0
last_log_time = time.time()

# 서버에서 클라이언트로부터 받은 영상을 처리하는 함수
def process_received_frame(frame: np.ndarray):
    """
    클라이언트로부터 받은 영상 프레임을 처리합니다.
    
    Args:
        frame: 받은 영상 프레임 (numpy array)
    
    Returns:
        처리된 영상 프레임
    """
    global frame_count, last_log_time
    
    # 함수 호출 확인용 즉시 로그 (매 프레임마다)
    print(f"[서버] 🔵 Handler 호출됨! 프레임 크기: {frame.shape if frame is not None else 'None'}")
    
    # 프레임이 None인 경우 처리
    if frame is None:
        print("[서버] ⚠️ 프레임이 None입니다!")
        return None
    
    # 프레임 카운터 증가
    frame_count += 1
    current_time = time.time()
    
    # 첫 프레임 수신 시 즉시 로그 출력
    if frame_count == 1:
        print(f"[서버] ✅ 첫 프레임 수신! 프레임 크기: {frame.shape}, 데이터 타입: {frame.dtype}")
        last_log_time = current_time
    
    # 1초마다 로그 출력
    if current_time - last_log_time >= 1.0:
        fps = frame_count / (current_time - last_log_time)
        print(f"[서버] 프레임 수신 중... 총 {frame_count}개 프레임 수신, FPS: {fps:.2f}")
        print(f"[서버] 프레임 크기: {frame.shape}, 데이터 타입: {frame.dtype}")
        frame_count = 0
        last_log_time = current_time
    
    # 영상에 서버 수신 확인 메시지 추가
    cv2.putText(frame, "SERVER RECEIVED: OK", (50, 50), 
                cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
    
    # 여기에 추가 영상 처리 로직을 추가할 수 있습니다
    # 예: 객체 감지, 필터 적용 등
    
    return frame

# FastAPI 앱 생성
app = FastAPI()

# FastRTC 스트림 설정 (서버는 send-receive 모드로 클라이언트로부터 영상을 받음)
print("[서버] Stream 객체 생성 중...")
print("[서버] Handler 함수:", process_received_frame)
print("[서버] Handler 함수 타입:", type(process_received_frame))

try:
    stream = Stream(
        handler=process_received_frame,
        modality="video",
        mode="send-receive",  # 클라이언트로부터 영상을 받고 처리된 영상을 반환
        allow_extra_tracks=True  # 추가 트랙 허용
    )
    print("[서버] Stream 객체 생성 완료")
    print("[서버] Stream 객체:", stream)
    print("[서버] Stream handler:", stream.handler if hasattr(stream, 'handler') else 'N/A')
except Exception as e:
    print(f"[서버] ❌ Stream 생성 오류: {e}")
    import traceback
    traceback.print_exc()
    raise

# FastRTC 스트림을 FastAPI 앱에 마운트
stream.mount(app)

# 클라이언트(Gradio UI 없이) 연결 시 기본 입력을 설정해 handler가 호출되도록 함
_initialized_webrtc_ids = set()

async def _ensure_default_input():
    while True:
        for webrtc_id, conns in list(stream.connections.items()):
            if webrtc_id in _initialized_webrtc_ids:
                continue
            if conns:
                stream.set_input(webrtc_id, None)
                _initialized_webrtc_ids.add(webrtc_id)
                print(f"[서버] 기본 입력 초기화 완료: {webrtc_id}")
        await asyncio.sleep(0.1)

@app.on_event("startup")
async def _startup_tasks():
    asyncio.create_task(_ensure_default_input())

# 클라이언트가 보내는 ICE 후보를 별도 엔드포인트에서도 처리
@app.post("/webrtc/ice")
async def webrtc_ice(request: Request):
    body = await request.json()
    body.setdefault("type", "ice-candidate")
    webrtc_id = body.get("webrtc_id")
    if not webrtc_id:
        return {"status": "failed", "meta": {"error": "missing_webrtc_id"}}
    return await stream.handle_offer(
        body, set_outputs=stream.set_additional_outputs(webrtc_id)
    )

# 루트 경로 추가 (상태 확인용)
@app.get("/")
async def root():
    return {
        "status": "running",
        "message": "FastRTC WebRTC 서버가 실행 중입니다",
        "webrtc_endpoint": "/webrtc/offer"
    }

# FastAPI 서버 실행
if __name__ == "__main__":
    print("=" * 50)
    print("WebRTC 서버가 시작되었습니다!")
    print("서버 주소: http://localhost:7860")
    print("클라이언트가 연결을 기다리는 중...")
    print("=" * 50)
    
    uvicorn.run(app, host="0.0.0.0", port=7860)
