// ===========================================================================
//  cpp_video_engine — native background-blur / alpha-compositing engine
// ===========================================================================

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace py = pybind11;

namespace {

using ByteArray  = py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast>;
using FloatArray = py::array_t<float,        py::array::c_style | py::array::forcecast>;

// Exact-enough integer divide by 255 for values in [0, 65535].
// Standard graphics trick: avoids a hardware divide in the inner loop.
inline std::uint8_t div255(int x) noexcept {
    return static_cast<std::uint8_t>((x + 1 + (x >> 8)) >> 8);
}

inline int make_odd(int v, int lo) noexcept {
    v = std::max(v, lo);
    return (v % 2 == 0) ? v + 1 : v;
}

// Wrap an HxWx3 uint8 NumPy buffer as a cv::Mat header. No copy.
cv::Mat wrap_frame(const ByteArray& arr, const char* who) {
    const auto info = arr.request();
    if (info.ndim != 3 || info.shape[2] != 3) {
        std::ostringstream oss;
        oss << who << ": expected an (H, W, 3) uint8 array, got ndim=" << info.ndim;
        throw std::invalid_argument(oss.str());
    }
    return cv::Mat(static_cast<int>(info.shape[0]),
                   static_cast<int>(info.shape[1]),
                   CV_8UC3,
                   info.ptr,
                   static_cast<std::size_t>(info.strides[0]));
}

// Wrap an HxW float32 mask as a cv::Mat header. No copy.
cv::Mat wrap_mask(const FloatArray& arr) {
    const auto info = arr.request();
    if (info.ndim == 3 && info.shape[2] == 1) {
        return cv::Mat(static_cast<int>(info.shape[0]),
                       static_cast<int>(info.shape[1]),
                       CV_32FC1, info.ptr,
                       static_cast<std::size_t>(info.strides[0]));
    }
    if (info.ndim != 2) {
        throw std::invalid_argument(
            "mask: expected an (H, W) or (H, W, 1) float32 array");
    }
    return cv::Mat(static_cast<int>(info.shape[0]),
                   static_cast<int>(info.shape[1]),
                   CV_32FC1, info.ptr,
                   static_cast<std::size_t>(info.strides[0]));
}

}  // namespace

// ---------------------------------------------------------------------------
//  HUD overlay — carried over from the original server/client preview windows,
//  which both drew a resolution/FPS line at (10, 30) in green at scale 0.7.
//  Kept out of VideoProcessor because it is orthogonal to the blur and you
//  will usually want it applied *after* processing, on the outgoing frame.
// ---------------------------------------------------------------------------
static void draw_hud(ByteArray& frame_arr,
                     const std::string& text,
                     int x, int y, double scale,
                     std::tuple<int, int, int> color,
                     int thickness, bool shadow)
{
    cv::Mat frame = wrap_frame(frame_arr, "frame");

    // Channel order follows the frame's own convention: (0, 255, 0) is green
    // in both BGR and RGB, which is why the original overlay colour survives
    // the switch to RGB unchanged.
    const cv::Scalar fg(std::get<0>(color), std::get<1>(color), std::get<2>(color));

    py::gil_scoped_release unlock;

    if (shadow) {
        // The original drew straight onto a sharp camera frame. Over a blurred
        // background the text can wash out, so lay down a dark outline first.
        cv::putText(frame, text, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX,
                    scale, cv::Scalar(0, 0, 0), thickness + 2, cv::LINE_AA);
    }
    cv::putText(frame, text, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX,
                scale, fg, thickness, cv::LINE_AA);
}

// ===========================================================================
//  VideoProcessor
// ===========================================================================
class VideoProcessor {
public:
    VideoProcessor(int blur_kernel   = 41,
                   double blur_sigma = 0.0,
                   double blur_scale = 0.25,
                   double threshold  = 0.5,
                   int feather       = 11,
                   bool hard_mask    = false)
    {
        set_blur_kernel(blur_kernel);
        set_blur_sigma(blur_sigma);
        set_blur_scale(blur_scale);
        set_threshold(threshold);
        set_feather(feather);
        hard_mask_ = hard_mask;
    }

    // --- tunables (safe to poke from the Streamlit thread between frames) ---
    void set_blur_kernel(int k) { blur_kernel_ = make_odd(k, 3); }
    void set_blur_sigma(double s) { blur_sigma_ = std::max(0.0, s); }
    void set_blur_scale(double s) { blur_scale_ = std::min(1.0, std::max(0.05, s)); }
    void set_threshold(double t) { threshold_ = std::min(1.0, std::max(0.0, t)); }
    void set_feather(int f) { feather_ = (f <= 1) ? 0 : make_odd(f, 3); }
    void set_hard_mask(bool v) { hard_mask_ = v; }

    int    blur_kernel() const { return blur_kernel_; }
    double blur_sigma()  const { return blur_sigma_; }
    double blur_scale()  const { return blur_scale_; }
    double threshold()   const { return threshold_; }
    int    feather()     const { return feather_; }
    bool   hard_mask()   const { return hard_mask_; }

    // Rolling cost of the native stage, in milliseconds.
    double last_ms() const { return last_ms_; }

    // -----------------------------------------------------------------------
    // Allocating variant: one output allocation, owned by NumPy.
    // -----------------------------------------------------------------------
    ByteArray process(const ByteArray& frame_in, const FloatArray& mask_in) {
        cv::Mat frame = wrap_frame(frame_in, "frame");
        cv::Mat mask  = wrap_mask(mask_in);

        ByteArray out(std::vector<py::ssize_t>{frame.rows, frame.cols, 3});
        cv::Mat out_mat(frame.rows, frame.cols, CV_8UC3, out.mutable_data());

        {
            py::gil_scoped_release unlock;   // no Python objects touched below
            run(frame, mask, out_mat);
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // Zero-allocation variant: writes into a buffer you already own.
    // -----------------------------------------------------------------------
    void process_into(const ByteArray& frame_in,
                      const FloatArray& mask_in,
                      ByteArray& out_arr)
    {
        cv::Mat frame = wrap_frame(frame_in, "frame");
        cv::Mat mask  = wrap_mask(mask_in);
        cv::Mat out   = wrap_frame(out_arr, "out");

        if (out.rows != frame.rows || out.cols != frame.cols) {
            throw std::invalid_argument("out: shape must match frame");
        }
        py::gil_scoped_release unlock;
        run(frame, mask, out);
    }

    // -----------------------------------------------------------------------
    // In-place variant: blurs the background directly inside `frame`.
    // -----------------------------------------------------------------------
    void process_inplace(ByteArray& frame_in, const FloatArray& mask_in) {
        cv::Mat frame = wrap_frame(frame_in, "frame");
        cv::Mat mask  = wrap_mask(mask_in);
        py::gil_scoped_release unlock;
        run(frame, mask, frame);
    }

private:
    // -----------------------------------------------------------------------
    // The actual pipeline. Called with the GIL released.
    // -----------------------------------------------------------------------
    void run(const cv::Mat& frame, const cv::Mat& mask, cv::Mat& out) {
        std::lock_guard<std::mutex> guard(mtx_);   // scratch buffers are shared
        const auto t0 = cv::getTickCount();

        const cv::Size sz = frame.size();

        // --- 1. background: blur at reduced resolution, then upscale ---------
        // A 41x41 Gaussian on 1280x720 is expensive; the same visual result at
        // 1/4 scale costs ~1/16 as much and the upscale hides the difference.
        const int sw = std::max(16, static_cast<int>(sz.width  * blur_scale_));
        const int sh = std::max(16, static_cast<int>(sz.height * blur_scale_));

        cv::resize(frame, small_, cv::Size(sw, sh), 0, 0, cv::INTER_AREA);

        const int k_small = make_odd(
            static_cast<int>(blur_kernel_ * blur_scale_), 3);
        const double sigma_small =
            (blur_sigma_ > 0.0) ? blur_sigma_ * blur_scale_ : 0.0;

        cv::GaussianBlur(small_, small_blurred_, cv::Size(k_small, k_small),
                         sigma_small, sigma_small, cv::BORDER_REPLICATE);

        cv::resize(small_blurred_, background_, sz, 0, 0, cv::INTER_LINEAR);

        // --- 2. alpha: float mask -> 8-bit, resized and feathered ------------
        cv::Mat mask_src = mask;
        if (mask.size() != sz) {
            cv::resize(mask, mask_resized_, sz, 0, 0, cv::INTER_LINEAR);
            mask_src = mask_resized_;
        }

        if (hard_mask_) {
            cv::threshold(mask_src, mask_bin_, threshold_, 1.0, cv::THRESH_BINARY);
            mask_bin_.convertTo(alpha_, CV_8UC1, 255.0);
        } else {
            // Soft ramp centred on the threshold: keeps hair/edges natural.
            const double gain   = 4.0;
            const double scale  = 255.0 * gain;
            const double offset = 255.0 * (0.5 - gain * threshold_);
            mask_src.convertTo(alpha_, CV_8UC1, scale, offset);
        }

        if (feather_ > 0) {
            cv::GaussianBlur(alpha_, alpha_, cv::Size(feather_, feather_), 0, 0,
                             cv::BORDER_REPLICATE);
        }

        // --- 3. composite: out = frame*a + background*(1-a) -------------------
        blend(frame, background_, alpha_, out);

        last_ms_ = 1000.0 * static_cast<double>(cv::getTickCount() - t0)
                   / cv::getTickFrequency();
    }

    static void blend(const cv::Mat& fg, const cv::Mat& bg,
                      const cv::Mat& alpha, cv::Mat& dst)
    {
        const int rows = fg.rows;
        const int cols = fg.cols;

        cv::parallel_for_(cv::Range(0, rows), [&](const cv::Range& r) {
            for (int y = r.start; y < r.end; ++y) {
                const std::uint8_t* fp = fg.ptr<std::uint8_t>(y);
                const std::uint8_t* bp = bg.ptr<std::uint8_t>(y);
                const std::uint8_t* ap = alpha.ptr<std::uint8_t>(y);
                std::uint8_t*       dp = dst.ptr<std::uint8_t>(y);

                for (int x = 0; x < cols; ++x) {
                    const int a  = ap[x];
                    const int ia = 255 - a;
                    const int i  = x * 3;
                    dp[i    ] = div255(fp[i    ] * a + bp[i    ] * ia);
                    dp[i + 1] = div255(fp[i + 1] * a + bp[i + 1] * ia);
                    dp[i + 2] = div255(fp[i + 2] * a + bp[i + 2] * ia);
                }
            }
        });
    }

    // tunables
    int    blur_kernel_ = 41;
    double blur_sigma_  = 0.0;
    double blur_scale_  = 0.25;
    double threshold_   = 0.5;
    int    feather_     = 11;
    bool   hard_mask_   = false;

    // scratch, reused across frames so steady-state allocation is zero
    cv::Mat small_, small_blurred_, background_;
    cv::Mat mask_resized_, mask_bin_, alpha_;

    std::mutex mtx_;
    double last_ms_ = 0.0;
};

// ===========================================================================
//  Bindings
// ===========================================================================
PYBIND11_MODULE(cpp_video_engine, m) {
    m.doc() = "Native OpenCV background-blur engine (pybind11, zero-copy).";

    m.attr("__version__") = "0.1.0";

    m.def("opencv_version", [] { return CV_VERSION; },
          "OpenCV version this module was compiled against.");

    m.def("num_threads", [] { return cv::getNumThreads(); },
          "Worker threads OpenCV will use for parallel_for_.");

    m.def("set_num_threads", [](int n) { cv::setNumThreads(n); }, py::arg("n"),
          "Cap OpenCV's thread pool (useful when several streams share a box).");

    m.def("draw_hud", &draw_hud,
          py::arg("frame"), py::arg("text"),
          py::arg("x") = 10, py::arg("y") = 30, py::arg("scale") = 0.7,
          py::arg("color") = std::make_tuple(0, 255, 0),
          py::arg("thickness") = 2, py::arg("shadow") = true,
          "Draw a status line onto `frame`, in place. Mirrors the overlay the "
          "original server/client preview windows drew with cv::putText.");

    py::class_<VideoProcessor>(m, "VideoProcessor", R"doc(
Background blur + alpha compositing, executed natively.

Frames are channel-order agnostic: feed BGR or RGB, get the same order back.
That lets the Python side hand MediaPipe an RGB frame straight from
`av.VideoFrame.to_ndarray(format="rgb24")` with no colour conversion at all.
)doc")
        .def(py::init<int, double, double, double, int, bool>(),
             py::arg("blur_kernel") = 41,
             py::arg("blur_sigma")  = 0.0,
             py::arg("blur_scale")  = 0.25,
             py::arg("threshold")   = 0.5,
             py::arg("feather")     = 11,
             py::arg("hard_mask")   = false)

        .def("process", &VideoProcessor::process,
             py::arg("frame"), py::arg("mask"),
             "Return a new (H, W, 3) uint8 array with the background blurred.")

        .def("process_into", &VideoProcessor::process_into,
             py::arg("frame"), py::arg("mask"), py::arg("out"),
             "Write into a caller-owned output buffer. Allocates nothing.")

        .def("process_inplace", &VideoProcessor::process_inplace,
             py::arg("frame"), py::arg("mask"),
             "Blur the background directly inside `frame`.")

        .def_property("blur_kernel", &VideoProcessor::blur_kernel,
                      &VideoProcessor::set_blur_kernel)
        .def_property("blur_sigma", &VideoProcessor::blur_sigma,
                      &VideoProcessor::set_blur_sigma)
        .def_property("blur_scale", &VideoProcessor::blur_scale,
                      &VideoProcessor::set_blur_scale)
        .def_property("threshold", &VideoProcessor::threshold,
                      &VideoProcessor::set_threshold)
        .def_property("feather", &VideoProcessor::feather,
                      &VideoProcessor::set_feather)
        .def_property("hard_mask", &VideoProcessor::hard_mask,
                      &VideoProcessor::set_hard_mask)
        .def_property_readonly("last_ms", &VideoProcessor::last_ms,
                               "Cost of the most recent native call, in ms.")

        .def("__repr__", [](const VideoProcessor& v) {
            std::ostringstream oss;
            oss << "<VideoProcessor kernel=" << v.blur_kernel()
                << " scale=" << v.blur_scale()
                << " threshold=" << v.threshold()
                << " feather=" << v.feather() << ">";
            return oss.str();
        });
}