from fastapi import FastAPI, Request, HTTPException
from fastapi import Query
import requests
import uvicorn
import time

app = FastAPI()

# C 서버(SFU)의 기본 주소
C_BASE_URL = "http://127.0.0.1:7860"

_ice_poll_count = 0
_ice_poll_last_ts = time.time()

def _log_ice_poll() -> None:
    global _ice_poll_count, _ice_poll_last_ts
    _ice_poll_count += 1
    now = time.time()
    if now - _ice_poll_last_ts >= 1.0:
        print(f"\r[turn_http] ICE poll req/s: {_ice_poll_count}    ", end="", flush=True)
        _ice_poll_count = 0
        _ice_poll_last_ts = now

@app.post("/webrtc/offer")
async def relay_offer(request: Request):
    data = await request.json()
    print("[*] Offer 중계 중...")
    try:
        # C 서버로 전달
        response = requests.post(f"{C_BASE_URL}/webrtc/offer", json=data, timeout=5)
        return response.json()
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"C 서버 연결 실패: {e}")

@app.post("/webrtc/ice")
async def relay_ice(request: Request):
    data = await request.json()
    print(f"[*] ICE 중계 중: {data.get('candidate', '')[:20]}...")
    try:
        # C 서버로 전달
        response = requests.post(f"{C_BASE_URL}/webrtc/ice", json=data, timeout=5)
        return response.json()
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"C 서버 연결 실패: {e}")

@app.get("/webrtc/ice")
async def relay_ice_poll(webrtc_id: str = Query(...)):
    try:
        _log_ice_poll()
        response = requests.get(
            f"{C_BASE_URL}/webrtc/ice", params={"webrtc_id": webrtc_id}, timeout=5
        )
        return response.json()
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"C 서버 연결 실패: {e}")

if __name__ == "__main__":
    # B 서버는 8000번 포트에서 A를 기다립니다.
    uvicorn.run(app, host="0.0.0.0", port=8000, access_log=False)
