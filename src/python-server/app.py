"""
Streamlit + WebRTC real-time background blur.

Pipeline per frame (all inside the WebRTC worker thread):

    Chrome --getUserMedia--> aiortc --> av.VideoFrame
        -> to_ndarray(format="rgb24")          # no colour conversion
        -> MediaPipe SelfieSegmentation        # person mask, float32 HxW
        -> cpp_video_engine.VideoProcessor     # native blur + alpha blend
        -> av.VideoFrame.from_ndarray(...)
        -> returned to Chrome (loopback)  AND  optionally pushed to the MCU

The MCU hop lives in its own asyncio loop on a background thread. If the MCU
is down, unreachable, or never configured, the local loopback is unaffected:
frames are dropped into a bounded queue that nobody is draining, and the
connector retries quietly with backoff.

Run:  streamlit run app.py
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import sys
import ctypes
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import av
import numpy as np
import streamlit as st

# Make sure the compiled module sitting next to this file is importable even if
# Streamlit is launched from somewhere else.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("blur-app")
logging.getLogger("aioice").setLevel(logging.WARNING)
logging.getLogger("aiortc").setLevel(logging.WARNING)

st.set_page_config(page_title="Native Background Blur", layout="wide")

# ---------------------------------------------------------------------------
# Native engine
# ---------------------------------------------------------------------------
try:
    import cpp_video_engine as cve
except ImportError as exc:  # pragma: no cover
    st.error(
        "Could not import `cpp_video_engine`.\n\n"
        f"```\n{exc}\n```\n"
        "Build it first:\n"
        "```bash\n"
        "cd ../cpp-lib\n"
        "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "
        "-DPython3_EXECUTABLE=$(which python)\n"
        "cmake --build build -j$(nproc)\n"
        "```\n"
        "If it built but still will not import, the module was almost certainly "
        "compiled against a different Python than the one running Streamlit — "
        "check that `-DPython3_EXECUTABLE` pointed at your venv."
    )
    st.stop()

from streamlit_webrtc import VideoProcessorBase, WebRtcMode, webrtc_streamer  # noqa: E402

# MediaPipe's legacy `mp.solutions.*` API was removed after 0.10.21; on 0.10.30+
# the module exposes only Image, ImageFormat and tasks. Everything below uses
# the supported Tasks API, which needs an explicit model asset.
import mediapipe as mp  # noqa: E402
from mediapipe.tasks import python as mp_python  # noqa: E402
from mediapipe.tasks.python import vision as mp_vision  # noqa: E402

MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/image_segmenter/"
    "selfie_segmenter/float16/latest/selfie_segmenter.tflite"
)
MODEL_PATH = Path(__file__).resolve().parent / "models" / "selfie_segmenter.tflite"


@st.cache_resource(show_spinner="Fetching the segmentation model...")
def ensure_model() -> str:
    """Download the segmenter once and cache it next to app.py."""
    if MODEL_PATH.exists() and MODEL_PATH.stat().st_size > 0:
        return str(MODEL_PATH)
    MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = MODEL_PATH.with_suffix(".part")
    urllib.request.urlretrieve(MODEL_URL, tmp)
    tmp.replace(MODEL_PATH)
    logger.info("Downloaded %s (%d bytes)", MODEL_PATH, MODEL_PATH.stat().st_size)
    return str(MODEL_PATH)


def check_mediapipe_runtime() -> Optional[str]:
    """
    Load MediaPipe's native library up front.

    `libmediapipe.so` carries hard NEEDED entries on libGLESv2.so.2 and
    libEGL.so.1 — unconditionally, even when you only ever ask for the CPU
    delegate — so on a headless WSL2 box it fails at dlopen. Doing the load
    here turns a raw OSError raised inside a WebRTC worker thread into an
    actionable message in the UI.
    """
    try:
        from importlib import resources
        lib = str(resources.files("mediapipe.tasks.c") / "libmediapipe.so")
    except Exception:  # noqa: BLE001
        return None    # older mediapipe with no C-bindings layer; nothing to check
    try:
        ctypes.CDLL(lib)
        return None
    except OSError as exc:
        return str(exc)


class Segmenter:
    """
    Thin wrapper over the Tasks ImageSegmenter.

    Replaces `mp.solutions.selfie_segmentation.SelfieSegmentation`. Two things
    differ from the legacy API and both matter here:

      * VIDEO running mode needs a strictly increasing timestamp per call, and
        raises if one ever goes backwards — hence the monotonic guard below.
      * the result carries a *list* of confidence masks rather than a single
        `segmentation_mask`, so the foreground channel has to be identified.
    """

    def __init__(self, model_path: str, use_gpu: bool = False):
        self.delegate_used = "CPU"
        self._seg = self._create(model_path, use_gpu)
        self._fg_index: Optional[int] = None
        self._ts = 0

    def _create(self, model_path: str, use_gpu: bool):
        def build(delegate):
            return mp_vision.ImageSegmenter.create_from_options(
                mp_vision.ImageSegmenterOptions(
                    base_options=mp_python.BaseOptions(
                        model_asset_path=model_path, delegate=delegate),
                    running_mode=mp_vision.RunningMode.VIDEO,
                    output_confidence_masks=True,
                    output_category_mask=False,
                )
            )

        if use_gpu:
            try:
                seg = build(mp_python.BaseOptions.Delegate.GPU)
                self.delegate_used = "GPU"
                return seg
            except Exception as exc:  # noqa: BLE001
                # The Linux pip wheel frequently ships without a working GPU
                # delegate. Falling back is far better than killing the stream.
                logger.warning("GPU delegate unavailable, using CPU: %s", exc)

        return build(mp_python.BaseOptions.Delegate.CPU)

    def _resolve_fg_index(self, n_masks: int) -> int:
        if n_masks == 1:
            return 0
        try:
            labels = [str(l).lower() for l in self._seg.labels]
        except Exception:  # noqa: BLE001
            labels = []
        for i, label in enumerate(labels[:n_masks]):
            if "background" not in label:
                return i
        return n_masks - 1

    def mask(self, rgb: np.ndarray) -> Optional[np.ndarray]:
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        # Strictly increasing, and immune to a clock that stalls within a ms.
        self._ts = max(self._ts + 1, int(time.monotonic() * 1000))

        result = self._seg.segment_for_video(image, self._ts)
        masks = result.confidence_masks
        if not masks:
            return None
        if self._fg_index is None:
            self._fg_index = self._resolve_fg_index(len(masks))
            logger.info("segmenter: %d masks, foreground index %d, %s delegate",
                        len(masks), self._fg_index, self.delegate_used)

        # (H, W, 1) float32, read-only. The engine takes it as-is with no copy.
        return masks[self._fg_index].numpy_view()

    def close(self) -> None:
        try:
            self._seg.close()
        except Exception:  # noqa: BLE001
            pass


# ===========================================================================
#  MCU forwarding (optional second hop)
# ===========================================================================
@dataclass
class MCUStatus:
    state: str = "disabled"      # disabled | connecting | connected | retrying | failed
    detail: str = ""
    frames_sent: int = 0
    frames_dropped: int = 0
    last_error: str = ""
    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)

    def set(self, **kwargs) -> None:
        with self._lock:
            for k, v in kwargs.items():
                setattr(self, k, v)

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "state": self.state,
                "detail": self.detail,
                "frames_sent": self.frames_sent,
                "frames_dropped": self.frames_dropped,
                "last_error": self.last_error,
            }


class MCUForwarder:
    """
    Pushes already-processed frames to an upstream WebRTC MCU.

    Fully decoupled from the loopback path:
      * `submit()` never blocks and never raises into the caller
      * the queue is bounded and drops the oldest frame when full
      * connection failures are retried on a backoff, forever, in the
        background thread — the UI just sees a status string change
    """

    QUEUE_DEPTH = 2

    def __init__(self, url: str, signaling: str = "json",
                 ice_servers: Optional[list] = None):
        self.url = url
        self.signaling = signaling          # "json" (aiortc-style) or "whip"
        self.ice_servers = ice_servers or ["stun:stun.l.google.com:19302"]

        self.status = MCUStatus(state="connecting")
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._queue: Optional[asyncio.Queue] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._ready = threading.Event()

    # ---- public, called from the WebRTC worker thread ----------------------
    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(
            target=self._thread_main, name="mcu-forwarder", daemon=True)
        self._thread.start()
        self._ready.wait(timeout=5.0)

    def submit(self, rgb: np.ndarray) -> None:
        """Non-blocking. Silently drops frames if the MCU is not keeping up."""
        loop, queue = self._loop, self._queue
        if loop is None or queue is None or loop.is_closed():
            return
        try:
            loop.call_soon_threadsafe(self._enqueue, rgb)
        except RuntimeError:
            pass  # loop shutting down

    def stop(self) -> None:
        self._stop.set()
        loop = self._loop
        if loop is not None and not loop.is_closed():
            loop.call_soon_threadsafe(loop.stop)

    # ---- internals, all on the background loop -----------------------------
    def _enqueue(self, rgb: np.ndarray) -> None:
        q = self._queue
        if q is None:
            return
        if q.full():
            try:
                q.get_nowait()
                self.status.set(frames_dropped=self.status.frames_dropped + 1)
            except asyncio.QueueEmpty:
                pass
        q.put_nowait(rgb)

    def _thread_main(self) -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self._loop = loop
        self._queue = asyncio.Queue(maxsize=self.QUEUE_DEPTH)
        self._ready.set()
        try:
            loop.run_until_complete(self._supervise())
        except Exception as exc:  # noqa: BLE001
            logger.exception("MCU forwarder crashed")
            self.status.set(state="failed", last_error=str(exc))
        finally:
            loop.close()

    async def _supervise(self) -> None:
        """Connect, and keep reconnecting, without ever propagating errors out."""
        backoff = 1.0
        while not self._stop.is_set():
            try:
                self.status.set(state="connecting", detail=self.url)
                await self._session()
                # Clean close: fall through and retry.
                self.status.set(state="retrying", detail="peer closed")
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001
                logger.warning("MCU connect failed: %s", exc)
                self.status.set(state="retrying", last_error=str(exc),
                                detail=f"retry in {backoff:.0f}s")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2.0, 30.0)

    async def _session(self) -> None:
        from aiortc import RTCConfiguration, RTCIceServer, RTCPeerConnection, \
            RTCSessionDescription

        config = RTCConfiguration(
            iceServers=[RTCIceServer(urls=u) for u in self.ice_servers])
        pc = RTCPeerConnection(configuration=config)
        track = _QueueVideoTrack(self._queue, self.status)
        pc.addTrack(track)

        closed = asyncio.Event()

        @pc.on("connectionstatechange")
        async def _on_state() -> None:
            state = pc.connectionState
            logger.info("MCU peer connection: %s", state)
            if state == "connected":
                self.status.set(state="connected", detail=self.url, last_error="")
            elif state in ("failed", "closed", "disconnected"):
                closed.set()

        try:
            await pc.setLocalDescription(await pc.createOffer())
            answer_sdp = await self._exchange(pc.localDescription.sdp)
            await pc.setRemoteDescription(
                RTCSessionDescription(sdp=answer_sdp, type="answer"))
            await closed.wait()
        finally:
            await pc.close()

    async def _exchange(self, offer_sdp: str) -> str:
        """
        Swap SDP with the MCU.

        Two conventions are supported out of the box; if your MCU speaks
        something else (Janus/mediasoup/LiveKit all have their own handshakes),
        this is the single method to replace.
        """
        import aiohttp

        timeout = aiohttp.ClientTimeout(total=8)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            if self.signaling == "whip":
                async with session.post(
                    self.url, data=offer_sdp,
                    headers={"Content-Type": "application/sdp"},
                ) as resp:
                    resp.raise_for_status()
                    return await resp.text()

            async with session.post(
                self.url,
                data=json.dumps({"sdp": offer_sdp, "type": "offer"}),
                headers={"Content-Type": "application/json"},
            ) as resp:
                resp.raise_for_status()
                payload = await resp.json()
                return payload["sdp"]


class _QueueVideoTrack:
    """Late-bound so aiortc is only imported when the MCU hop is actually used."""

    def __new__(cls, queue, status):
        from aiortc import VideoStreamTrack

        class _Track(VideoStreamTrack):
            kind = "video"

            def __init__(self) -> None:
                super().__init__()
                self._q = queue
                self._status = status
                self._last: Optional[np.ndarray] = None

            async def recv(self) -> av.VideoFrame:
                pts, time_base = await self.next_timestamp()
                try:
                    arr = await asyncio.wait_for(self._q.get(), timeout=1.0)
                    self._last = arr
                except asyncio.TimeoutError:
                    # Local camera paused or the callback is starving us.
                    # Repeat the last frame so the MCU sees a live stream.
                    arr = self._last
                    if arr is None:
                        arr = np.zeros((360, 640, 3), dtype=np.uint8)

                frame = av.VideoFrame.from_ndarray(arr, format="rgb24")
                frame.pts, frame.time_base = pts, time_base
                self._status.set(frames_sent=self._status.frames_sent + 1)
                return frame

        return _Track()


# ===========================================================================
#  Frame processor
# ===========================================================================
class BlurProcessor(VideoProcessorBase):
    def __init__(self, model_path: str, use_gpu: bool = False) -> None:
        self.engine = cve.VideoProcessor(
            blur_kernel=41, blur_scale=0.25, threshold=0.5, feather=11)

        # One segmenter per stream. The Tasks graph is not thread-safe, but
        # streamlit-webrtc gives each stream its own worker thread.
        self.segmenter = Segmenter(model_path, use_gpu=use_gpu)

        # First call into the engine costs 10-45 ms (lazy init + scratch
        # allocation). Pay it here rather than on the viewer's first frame.
        warm = np.zeros((72, 128, 3), np.uint8)
        self.engine.process(warm, np.zeros((72, 128), np.float32))

        self.invert_mask = False

        self.forwarder: Optional[MCUForwarder] = None
        self.bypass = False
        self.hud = False

        self._native_ms = 0.0
        self._total_ms = 0.0
        self._fps = 0.0
        self._t_prev = time.perf_counter()
        self._stats_lock = threading.Lock()

    # -- called from the Streamlit script thread ----------------------------
    def configure(self, *, blur_kernel: int, blur_scale: float,
                  threshold: float, feather: int, hard_mask: bool,
                  bypass: bool, hud: bool, invert_mask: bool) -> None:
        e = self.engine
        e.blur_kernel = blur_kernel
        e.blur_scale = blur_scale
        e.threshold = threshold
        e.feather = feather
        e.hard_mask = hard_mask
        self.bypass = bypass
        self.hud = hud
        self.invert_mask = invert_mask

    def attach_mcu(self, url: str, signaling: str) -> None:
        if self.forwarder is not None:
            return
        self.forwarder = MCUForwarder(url, signaling=signaling)
        self.forwarder.start()

    def stats(self) -> dict:
        with self._stats_lock:
            return {"native_ms": self._native_ms,
                    "total_ms": self._total_ms,
                    "fps": self._fps}

    # -- called from the WebRTC worker thread -------------------------------
    def recv(self, frame: av.VideoFrame) -> av.VideoFrame:
        t_start = time.perf_counter()

        # RGB throughout: MediaPipe wants RGB and the native engine is
        # channel-order agnostic, so there is not a single cvtColor in the
        # hot path.
        rgb = frame.to_ndarray(format="rgb24")

        if self.bypass:
            out = rgb
        else:
            mask = self.segmenter.mask(rgb)
            if mask is None:
                out = rgb
            else:
                if self.invert_mask:
                    mask = 1.0 - mask
                # Zero-copy in, one allocation out.
                out = self.engine.process(rgb, mask)

        # The overlay the original preview windows drew, minus the JPEG quality
        # field — WebRTC owns bitrate adaptation now, so there is no quality
        # knob of ours left to report.
        if self.hud:
            if out is rgb:
                out = rgb.copy()   # never scribble on the source frame's buffer
            cve.draw_hud(
                out,
                f"{out.shape[1]}x{out.shape[0]} | "
                f"{self._fps:.1f} fps | native {self.engine.last_ms:.1f} ms",
            )

        # Hand the same pixels to the MCU. A fresh VideoFrame is built on the
        # far side so the two encoders never share a mutable frame object.
        if self.forwarder is not None:
            self.forwarder.submit(out)

        new_frame = av.VideoFrame.from_ndarray(out, format="rgb24")
        new_frame.pts = frame.pts
        new_frame.time_base = frame.time_base

        now = time.perf_counter()
        with self._stats_lock:
            self._native_ms = self.engine.last_ms
            self._total_ms = (now - t_start) * 1000.0
            dt = now - self._t_prev
            if dt > 0:
                self._fps = 0.9 * self._fps + 0.1 * (1.0 / dt)
            self._t_prev = now

        return new_frame


# ===========================================================================
#  UI
# ===========================================================================
st.title("Real-time background blur")
st.caption(
    f"OpenCV {cve.opencv_version()} · engine v{cve.__version__} · "
    f"{cve.num_threads()} native threads"
)

with st.sidebar:
    st.header("Blur")
    blur_kernel = st.slider("Kernel size", 3, 121, 41, step=2,
                            help="Gaussian kernel at full resolution.")
    blur_scale = st.slider("Blur working scale", 0.10, 1.0, 0.25, step=0.05,
                           help="Blur is computed at this fraction of the frame "
                                "size, then upscaled. Lower is much faster.")

    st.header("Mask")
    threshold = st.slider("Threshold", 0.0, 1.0, 0.50, step=0.05)
    feather = st.slider("Edge feather", 0, 41, 11, step=2)
    hard_mask = st.checkbox("Hard cut-out", value=False,
                            help="Binary mask instead of a soft ramp.")
    invert_mask = st.checkbox("Invert mask", value=False,
                              help="Flip foreground/background if the blur "
                                   "lands on you instead of the room.")

    use_gpu = st.checkbox("Try GPU delegate", value=False,
                          help="Falls back to CPU if the wheel has no working "
                               "GPU delegate, which is common on Linux.")

    bypass = st.checkbox("Bypass processing", value=False,
                         help="Pass frames straight through, for A/B timing.")
    hud = st.checkbox("HUD overlay", value=False,
                      help="Burn resolution/fps/timing into the frame, as the "
                           "original preview windows did.")

    st.header("Resolution")
    width = st.selectbox("Capture width", [640, 960, 1280, 1920], index=2)

    st.header("Upstream MCU")
    mcu_enabled = st.checkbox("Forward to MCU", value=False)
    mcu_url = st.text_input("Signaling endpoint",
                            value="http://127.0.0.1:8080/offer",
                            disabled=not mcu_enabled)
    mcu_signaling = st.radio("Handshake", ["json", "whip"], horizontal=True,
                             disabled=not mcu_enabled)

RTC_CONFIG = {"iceServers": [{"urls": ["stun:stun.l.google.com:19302"]}]}

MEDIA_CONSTRAINTS = {
    "video": {
        "width": {"ideal": width},
        "height": {"ideal": int(width * 9 / 16)},
        "frameRate": {"ideal": 30},
    },
    "audio": False,
}

_runtime_problem = check_mediapipe_runtime()
if _runtime_problem:
    st.error(
        "MediaPipe's native library will not load.\n\n"
        f"```\n{_runtime_problem}\n```\n"
        "`libmediapipe.so` links against OpenGL ES and EGL even for CPU-only "
        "inference, and a headless WSL2 install has neither. Fix:\n\n"
        "```bash\nsudo apt install -y libgles2 libegl1\n```"
    )
    st.stop()

try:
    model_path = ensure_model()
except Exception as exc:  # noqa: BLE001
    st.error(
        f"Could not fetch the segmentation model.\n\n```\n{exc}\n```\n"
        f"Download it manually to `{MODEL_PATH}`:\n\n"
        f"```bash\nmkdir -p {MODEL_PATH.parent}\n"
        f"curl -L -o {MODEL_PATH} \\\n  {MODEL_URL}\n```"
    )
    st.stop()

col_video, col_stats = st.columns([3, 1])

with col_video:
    ctx = webrtc_streamer(
        key="blur",
        mode=WebRtcMode.SENDRECV,
        rtc_configuration=RTC_CONFIG,
        media_stream_constraints=MEDIA_CONSTRAINTS,
        video_processor_factory=lambda: BlurProcessor(model_path, use_gpu),
        async_processing=True,
    )

if ctx.video_processor:
    ctx.video_processor.configure(
        blur_kernel=blur_kernel,
        blur_scale=blur_scale,
        threshold=threshold,
        feather=feather,
        hard_mask=hard_mask,
        bypass=bypass,
        hud=hud,
        invert_mask=invert_mask,
    )
    if mcu_enabled and mcu_url:
        ctx.video_processor.attach_mcu(mcu_url, mcu_signaling)

with col_stats:
    st.subheader("Timing")
    if ctx.video_processor and ctx.state.playing:
        s = ctx.video_processor.stats()
        st.metric("Native stage", f"{s['native_ms']:.2f} ms")
        st.metric("Full callback", f"{s['total_ms']:.2f} ms")
        st.metric("Throughput", f"{s['fps']:.1f} fps")
        st.caption(f"segmenter delegate: {ctx.video_processor.segmenter.delegate_used}")
    else:
        st.info("Press START to begin.")

    st.subheader("MCU")
    if ctx.video_processor and ctx.video_processor.forwarder:
        snap = ctx.video_processor.forwarder.status.snapshot()
        badge = {"connected": "🟢", "connecting": "🟡",
                 "retrying": "🟠", "failed": "🔴"}.get(snap["state"], "⚪")
        st.write(f"{badge} **{snap['state']}** — {snap['detail']}")
        st.write(f"sent {snap['frames_sent']} · dropped {snap['frames_dropped']}")
        if snap["last_error"]:
            st.caption(snap["last_error"])
    else:
        st.write("⚪ not attached")

    if st.button("Refresh stats"):
        st.rerun()

st.markdown(
    "---\n"
    "**Note:** the loopback view above is independent of the MCU hop. "
    "If the MCU is offline the connector retries in the background on a "
    "capped exponential backoff and frames destined for it are dropped, "
    "but the local preview keeps running at full rate."
)