"""
Verify the native engine in isolation, before WebRTC is in the picture.

    python smoke_test.py

If this passes, any remaining problem is in the browser/aiortc layer, not
in the C++ module or the bindings.
"""
import sys
import time

import numpy as np

try:
    import cpp_video_engine as cve
except ImportError as exc:
    sys.exit(f"cpp_video_engine not importable: {exc}\n"
             "Build it: cd ../cpp-lib && cmake --build build -j$(nproc)")

print(f"engine   : v{cve.__version__}")
print(f"opencv   : {cve.opencv_version()}")
print(f"threads  : {cve.num_threads()}")

H, W = 720, 1280
rng = np.random.default_rng(0)
frame = rng.integers(0, 255, (H, W, 3), dtype=np.uint8)
mask = np.zeros((H, W), np.float32)
mask[200:600, 400:900] = 1.0          # stand-in for a person

proc = cve.VideoProcessor()
print(f"processor: {proc!r}")

out = proc.process(frame, mask)
assert out.shape == (H, W, 3) and out.dtype == np.uint8
assert np.array_equal(out[350:450, 550:750], frame[350:450, 550:750]), \
    "foreground was modified — mask polarity is inverted"
assert not np.array_equal(out[:50, :50], frame[:50, :50]), \
    "background was not blurred"

# MediaPipe's landscape model returns 256x144; the engine rescales for you.
low_res = np.zeros((144, 256), np.float32)
low_res[40:120, 80:200] = 1.0
assert proc.process(frame, low_res).shape == (H, W, 3)

# Zero-allocation and in-place paths must agree with the allocating one.
buf = np.empty_like(frame)
proc.process_into(frame, mask, buf)
assert np.array_equal(buf, out)

inplace = frame.copy()
proc.process_inplace(inplace, mask)
assert np.array_equal(inplace, out)

# Benchmark
bench = cve.VideoProcessor(blur_kernel=41, blur_scale=0.25)
for _ in range(5):
    bench.process(frame, mask)

N = 100
t0 = time.perf_counter()
for _ in range(N):
    bench.process(frame, mask)
per_frame = (time.perf_counter() - t0) / N * 1000

budget = 1000 / 30
print(f"\n720p native stage: {per_frame:.2f} ms/frame "
      f"({per_frame / budget:.0%} of a 30 fps budget)")
print("ALL CHECKS PASSED")

# HUD overlay (the cv::putText carried over from the original preview windows)
hud_frame = out.copy()
cve.draw_hud(hud_frame, f"{W}x{H} | 30.0 fps | native {per_frame:.1f} ms")
assert not np.array_equal(hud_frame, out), "draw_hud drew nothing"
print("HUD overlay: OK")
