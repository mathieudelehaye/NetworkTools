# Native background blur — build & run (WSL2 / Ubuntu)

Target layout:

```
/home/mdelehaye/cpp/NetworkTools/src/
├── cpp-lib/
│   ├── vcpkg.json
│   ├── CMakeLists.txt
│   ├── native_engine.cpp
│   └── build/                  # created by cmake
└── python-server/
    ├── app.py
    ├── smoke_test.py
    ├── requirements.txt
    └── cpp_video_engine.cpython-3XX-x86_64-linux-gnu.so   # build output
```

## 0. System packages

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar \
                    pkg-config python3-dev python3-venv nasm
```

## 1. vcpkg (once)

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
echo 'export VCPKG_ROOT=$HOME/vcpkg' >> ~/.bashrc
export VCPKG_ROOT=$HOME/vcpkg
```

## 2. Python environment first

Do this **before** configuring CMake — the extension module must be compiled
against the same interpreter that will import it.

```bash
cd /home/mdelehaye/cpp/NetworkTools/src/python-server
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
```

## 3. Build the C++ engine

```bash
cd /home/mdelehaye/cpp/NetworkTools/src/cpp-lib

cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPython3_EXECUTABLE=$(which python)

cmake --build build -j$(nproc)
```

The first configure triggers vcpkg manifest mode, which compiles OpenCV from
source: **20–40 minutes**. Subsequent builds are seconds. Watch for
`Module output -> .../python-server` in the configure log.

## 4. Verify the module in isolation

```bash
cd ../python-server
python smoke_test.py
```

Expect `ALL CHECKS PASSED` and a per-frame timing figure.

## 5. Run

```bash
streamlit run app.py
```

Open <http://localhost:8501> in Chrome on Windows and press **START**.
WSL2 forwards `localhost`, and Chrome treats `localhost` as a secure origin,
so `getUserMedia` works without TLS.

## Rebuild loop

Editing `native_engine.cpp`:

```bash
cmake --build build -j$(nproc)
```

Streamlit will **not** pick up a new `.so` on hot-reload — Python caches the
extension module. Stop and restart `streamlit run` after every native build.

## MCU hop

Tick **Forward to MCU** in the sidebar and give it a signalling endpoint. Two
handshakes are built in:

- `json` — POST `{"sdp": ..., "type": "offer"}`, expects `{"sdp": ...}` back.
  This is the convention used by the aiortc server examples.
- `whip` — POST the raw SDP with `Content-Type: application/sdp`, answer comes
  back as the response body.

Anything else (Janus, mediasoup, LiveKit) needs `MCUForwarder._exchange()`
rewritten; that method is the only place the wire protocol appears.

With the MCU offline the connector retries on a capped exponential backoff and
the sidebar shows `retrying`. The local preview is unaffected.

---

## What was carried over from the original sources

The full OpenCV surface of the old client/server was: `VideoCapture`,
`imencode`/`imdecode`, `imshow`/`namedWindow`/`waitKey`, `imwrite`, and
`putText`. There is no `cvtColor`, no `resize`, and no filtering anywhere in
the originals — which is why the pipeline could be switched to RGB end to end
with nothing to port.

| Original | Fate |
| --- | --- |
| `VideoCapture(0, CAP_DSHOW)`, MJPG 1920x1080 | replaced by `getUserMedia` constraints |
| `imencode(".jpg", ...)` @ quality 60-85 | replaced by the WebRTC codec |
| adaptive quality on `consecutive_errors` | replaced by WebRTC congestion control |
| 58 KB chunking + 12-byte header | replaced by RTP |
| client reassembly map + JPEG end-marker scan | replaced by RTP sequencing |
| `imdecode(..., IMREAD_COLOR)` | replaced by the WebRTC decoder |
| `imshow` / `namedWindow` / `waitKey` | replaced by the browser `<video>` element |
| `imwrite` debug frame dumps | dropped (use browser devtools) |
| **`putText` resolution/FPS overlay** | **kept — `cpp_video_engine.draw_hud()`** |
| windowed FPS counter over 30 frames | kept, as an EWMA in `BlurProcessor` |
| `Sleep((FRAME_TIME - duration) / 2)` pacing | replaced by WebRTC's pacer |
