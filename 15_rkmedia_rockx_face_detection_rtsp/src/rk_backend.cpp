#include "vision/rk_backend.hpp"
#ifdef RKAIQ
#include <rk_aiq_user_api_imgproc.h>
#include <rk_aiq_user_api_sysctl.h>
#endif
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vision {
namespace {
void warn_rk(int status, const char* operation) noexcept {
    if (status) std::fprintf(stderr, "cleanup: %s failed (%d)\n", operation, status);
}
// Bracket CPU reads of DMA memory, even though the caller never copies pixels.
class CpuRead : Noncopyable {
public:
    explicit CpuRead(MEDIA_BUFFER buffer) : buffer_(buffer) {
        check_rk(RK_MPI_MB_BeginCPUAccess(buffer_, RK_TRUE), "MB BeginCPUAccess");
    }
    ~CpuRead() { warn_rk(RK_MPI_MB_EndCPUAccess(buffer_, RK_TRUE), "MB EndCPUAccess"); }
private:
    MEDIA_BUFFER buffer_;
};
} // namespace

void check_rk(int status, const char* operation) {
    if (status) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(status) + ")");
}

MediaBuffer::MediaBuffer(MEDIA_BUFFER buffer) : buffer_(buffer) {
    if (!buffer_) throw std::runtime_error("null MEDIA_BUFFER");
}
MediaBuffer::~MediaBuffer() { warn_rk(RK_MPI_MB_ReleaseBuffer(buffer_), "MB ReleaseBuffer"); }
int MediaBuffer::fd() const {
    const int fd = RK_MPI_MB_GetFD(buffer_);
    if (fd < 0) throw std::runtime_error("buffer is not backed by DMA memory");
    return fd;
}
uint64_t MediaBuffer::pts() const { return RK_MPI_MB_GetTimestamp(buffer_); }
MB_IMAGE_INFO_S MediaBuffer::image_info() const {
    MB_IMAGE_INFO_S info{};
    check_rk(RK_MPI_MB_GetImageInfo(buffer_, &info), "MB GetImageInfo");
    if (!info.u32Width || !info.u32Height) throw std::runtime_error("empty image");
    return info;
}
namespace {
Frame adopt(MEDIA_BUFFER mb) {
    if (!mb) return {};
    try { return std::make_shared<MediaBuffer>(mb); }
    catch (...) { RK_MPI_MB_ReleaseBuffer(mb); throw; }
}
} // namespace
Frame get_buffer(MOD_ID_E module, int channel, int timeout_ms) {
    return adopt(RK_MPI_SYS_GetMediaBuffer(module, channel, timeout_ms));
}
MediaSystem::MediaSystem() { check_rk(RK_MPI_SYS_Init(), "SYS Init"); }
// This RKMedia API has no SYS_Exit. Channel owners perform teardown.

Isp::Isp(const Config& c) : camera_(c.camera) {
    if (c.iq_dir.empty()) return;
#ifdef RKAIQ
    // RKAiq uses HDR_MODE during initialization even though prepare() also
    // receives the working mode. This process owns a single normal-mode ISP.
    check_rk(::setenv("HDR_MODE", "0", 1), "ISP set HDR_MODE");
    rk_aiq_static_info_t info{};
    rk_aiq_uapi_sysctl_enumStaticMetas(camera_, &info);
    const char* sensor_name = info.sensor_info.sensor_name;
    if (!sensor_name || !*sensor_name)
        throw std::runtime_error("ISP camera has no RKAiq static metadata");
    auto* context = rk_aiq_uapi_sysctl_init(sensor_name,
                                            c.iq_dir.c_str(), nullptr, nullptr);
    if (!context) throw std::runtime_error("ISP Init returned a null RKAiq context");
    context_ = context;
    try {
        check_rk(rk_aiq_uapi_sysctl_prepare(context, 0, 0,
                                            RK_AIQ_WORKING_MODE_NORMAL),
                 "ISP Prepare");
        check_rk(rk_aiq_uapi_sysctl_start(context), "ISP Start");
        started_ = true;
        frameRateInfo_t rate{};
        rate.mode = OP_MANUAL;
        rate.fps = c.fps;
        check_rk(rk_aiq_uapi_setFrameRate(context, rate), "ISP SetFrameRate");
    } catch (...) {
        if (started_) rk_aiq_uapi_sysctl_stop(context, false);
        rk_aiq_uapi_sysctl_deinit(context);
        context_ = nullptr;
        started_ = false;
        throw;
    }
#else
    throw std::runtime_error("ISP support disabled at build time; use --no-isp with an externally managed ISP");
#endif
}
Isp::~Isp() {
#ifdef RKAIQ
    auto* context = static_cast<rk_aiq_sys_ctx_t*>(context_);
    if (context) {
        if (started_) rk_aiq_uapi_sysctl_stop(context, false);
        rk_aiq_uapi_sysctl_deinit(context);
    }
#endif
}

ViChannel::ViChannel(int camera, int channel, std::string node, int width, int height, int buffers)
    : camera_(camera), channel_(channel), node_(std::move(node)) {
    VI_CHN_ATTR_S attr{};
    attr.pcVideoNode = const_cast<char*>(node_.c_str());
    attr.u32BufCnt = buffers;
    attr.u32Width = width; attr.u32Height = height;
    attr.enPixFmt = IMAGE_TYPE_NV12;
    // Ask the V4L2 capture flow for V4L2_MEMORY_DMABUF. RKMedia then carries
    // the same hardware-backed buffers across VI/RGA bindings without a CPU
    // mmap/copy stage on the main video path.
    attr.enBufType = VI_CHN_BUF_TYPE_DMA;
    attr.enWorkMode = VI_WORK_MODE_NORMAL;
    check_rk(RK_MPI_VI_SetChnAttr(camera_, channel_, &attr), "VI SetChnAttr");
    try {
        check_rk(RK_MPI_VI_EnableChn(camera_, channel_), "VI EnableChn");
    } catch (...) {
        // SetChnAttr moves this RKMedia generation to READY. Disable also
        // closes READY channels, so a failed enable cannot poison a restart.
        warn_rk(RK_MPI_VI_DisableChn(camera_, channel_), "VI rollback DisableChn");
        throw;
    }
}
ViChannel::~ViChannel() { warn_rk(RK_MPI_VI_DisableChn(camera_, channel_), "VI DisableChn"); }
MPP_CHN_S ViChannel::endpoint() const { return {RK_ID_VI, camera_, channel_}; }

RgaChannel::RgaChannel(int channel, int width, int height, IMAGE_TYPE_E output, int rotation, int buffers)
    : channel_(channel) {
    RGA_ATTR_S attr{};
    attr.bEnBufPool = RK_TRUE; attr.u16BufPoolCnt = buffers; attr.u16Rotaion = rotation;
    attr.stImgIn.imgType = IMAGE_TYPE_NV12;
    attr.stImgIn.u32Width = width; attr.stImgIn.u32Height = height;
    attr.stImgIn.u32HorStride = width; attr.stImgIn.u32VirStride = height;
    const int out_w = rotation % 180 ? height : width;
    const int out_h = rotation % 180 ? width : height;
    attr.stImgOut.imgType = output;
    attr.stImgOut.u32Width = out_w; attr.stImgOut.u32Height = out_h;
    attr.stImgOut.u32HorStride = out_w; attr.stImgOut.u32VirStride = out_h;
    check_rk(RK_MPI_RGA_CreateChn(channel_, &attr), "RGA CreateChn");
}
RgaChannel::~RgaChannel() { warn_rk(RK_MPI_RGA_DestroyChn(channel_), "RGA DestroyChn"); }
MPP_CHN_S RgaChannel::endpoint() const { return {RK_ID_RGA, 0, channel_}; }
Binding::Binding(MPP_CHN_S source, MPP_CHN_S destination) : source_(source), destination_(destination) {
    check_rk(RK_MPI_SYS_Bind(&source_, &destination_), "SYS Bind");
}
Binding::~Binding() { warn_rk(RK_MPI_SYS_UnBind(&source_, &destination_), "SYS UnBind"); }

VencChannel::VencChannel(int channel, int width, int height, int stride, int vstride, int fps, int bitrate)
    : channel_(channel) {
    VENC_CHN_ATTR_S attr{};
    attr.stVencAttr.enType = RK_CODEC_TYPE_H264;
    attr.stVencAttr.imageType = IMAGE_TYPE_NV12;
    attr.stVencAttr.u32PicWidth = width; attr.stVencAttr.u32PicHeight = height;
    attr.stVencAttr.u32VirWidth = stride; attr.stVencAttr.u32VirHeight = vstride;
    attr.stVencAttr.u32Profile = 66; // baseline: no B-frame reordering
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    auto& rc = attr.stRcAttr.stH264Cbr;
    rc.u32Gop = fps; rc.u32BitRate = bitrate;
    rc.fr32DstFrameRateDen = 1; rc.fr32DstFrameRateNum = fps;
    rc.u32SrcFrameRateDen = 1; rc.u32SrcFrameRateNum = fps;
    check_rk(RK_MPI_VENC_CreateChn(channel_, &attr), "VENC CreateChn");
}
VencChannel::~VencChannel() { warn_rk(RK_MPI_VENC_DestroyChn(channel_), "VENC DestroyChn"); }
void VencChannel::send(const Frame& frame) {
    check_rk(RK_MPI_SYS_SendMediaBuffer(RK_ID_VENC, channel_, frame->get()), "VENC SendMediaBuffer");
}

HardwareCompositor::HardwareCompositor(int rga_channel, bool enabled)
    : channel_(rga_channel), enabled_(enabled) {
    if (!enabled_) disable_all();
}
HardwareCompositor::~HardwareCompositor() { disable_all(); }
void HardwareCompositor::disable_all() noexcept {
    for (int id = 0; id < 8; ++id) {
        OSD_REGION_INFO_S region{};
        region.enRegionId = static_cast<OSD_REGION_ID_E>(id);
        region.u8Enable = 0;
        warn_rk(RK_MPI_RGA_RGN_SetCover(channel_, &region, nullptr),
                "RGA disable cover");
    }
}
std::size_t HardwareCompositor::update(const std::vector<Box>& boxes) {
    if (!enabled_) return 0;
    const std::size_t count = std::min<std::size_t>(boxes.size(), 2);
    int region_id = 0;
    COVER_INFO_S cover{};
    cover.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
    cover.u32Color = 0xff00ff00; // opaque green
    for (std::size_t i = 0; i < count; ++i) {
        const Box& b = boxes[i];
        const int width = b.right - b.left;
        const int height = b.bottom - b.top;
        const int thickness = std::min(4, std::min(width, height));
        if (thickness <= 0) continue;
        const int inner_height = std::max(0, height - 2 * thickness);
        const int rects[4][4] = {
            {b.left, b.top, width, thickness},
            {b.left, b.bottom - thickness, width, thickness},
            {b.left, b.top + thickness, thickness, inner_height},
            {b.right - thickness, b.top + thickness, thickness, inner_height}
        };
        for (const auto& rect : rects) {
            OSD_REGION_INFO_S region{};
            region.enRegionId = static_cast<OSD_REGION_ID_E>(region_id++);
            region.u32PosX = static_cast<RK_U32>(rect[0]);
            region.u32PosY = static_cast<RK_U32>(rect[1]);
            region.u32Width = static_cast<RK_U32>(rect[2]);
            region.u32Height = static_cast<RK_U32>(rect[3]);
            region.u8Enable = rect[2] > 0 && rect[3] > 0;
            check_rk(RK_MPI_RGA_RGN_SetCover(channel_, &region, &cover),
                     "RGA set box edge");
        }
    }
    while (region_id < 8) {
        OSD_REGION_INFO_S region{};
        region.enRegionId = static_cast<OSD_REGION_ID_E>(region_id++);
        region.u8Enable = 0;
        check_rk(RK_MPI_RGA_RGN_SetCover(channel_, &region, nullptr),
                 "RGA clear box edge");
    }
    return count;
}
FaceDetector::FaceDetector(const Config& config) : threshold_(config.threshold) {
    auto* settings = rockx_create_config();
    if (!settings) throw std::runtime_error("RockX config allocation failed");
    try {
        check_rk(rockx_add_config(settings, ROCKX_CONFIG_DATA_PATH, config.rockx_dir.c_str()), "RockX config path");
        check_rk(rockx_create(&detector_, ROCKX_MODULE_FACE_DETECTION, settings, sizeof(*settings)), "RockX detector create");
        check_rk(rockx_create(&filter_, ROCKX_MODULE_FACE_LANDMARK_5, settings, sizeof(*settings)), "RockX filter create");
    } catch (...) {
        if (filter_) rockx_destroy(filter_);
        if (detector_) rockx_destroy(detector_);
        rockx_release_config(settings);
        throw;
    }
    rockx_release_config(settings);
}
FaceDetector::~FaceDetector() {
    if (filter_) warn_rk(rockx_destroy(filter_), "RockX filter destroy");
    if (detector_) warn_rk(rockx_destroy(detector_), "RockX detector destroy");
}
Detection FaceDetector::infer(const Frame& frame) {
    const auto info = frame->image_info();
    // RockX's pointer API does not express row stride. Never pass a padded RGB image.
    if (info.enImgType != IMAGE_TYPE_BGR888 || info.u32HorStride != info.u32Width ||
        info.u32VerStride != info.u32Height ||
        RK_MPI_MB_GetSize(frame->get()) < size_t(info.u32Width) * info.u32Height * 3)
        throw std::runtime_error("RockX needs a tightly packed BGR888 substream");
    CpuRead access(frame->get());
    rockx_image_t image{};
    image.width = info.u32Width; image.height = info.u32Height;
    image.pixel_format = ROCKX_PIXEL_FORMAT_BGR888;
    // RockX borrows the RKMedia mapping for this synchronous call. Marking the
    // image as preallocated prevents the SDK from treating DMA-backed storage
    // as an image allocation that it owns.
    image.is_prealloc_buf = 1;
    image.data = static_cast<uint8_t*>(RK_MPI_MB_GetPtr(frame->get()));
    image.size = size_t(info.u32Width) * info.u32Height * 3;
    if (!image.data) throw std::runtime_error("RockX input mapping failed");
    rockx_object_array_t objects{};
    check_rk(rockx_face_detect(detector_, &image, &objects, nullptr), "RockX face detect");
    Detection result;
    result.pts_us = frame->pts(); result.width = image.width; result.height = image.height;
    const int capacity = sizeof(objects.object) / sizeof(objects.object[0]);
    if (objects.count < 0 || objects.count > capacity) throw std::runtime_error("invalid RockX object count");
    for (int i = 0; i < objects.count; ++i) {
        auto& face = objects.object[i];
        if (!std::isfinite(face.score) || face.score < threshold_) continue;
        // Filter input must also be clipped; hardware/model results can extend outside the image.
        face.box.left = std::max(0, face.box.left); face.box.top = std::max(0, face.box.top);
        face.box.right = std::min(static_cast<int>(image.width), face.box.right);
        face.box.bottom = std::min(static_cast<int>(image.height), face.box.bottom);
        if (face.box.right <= face.box.left || face.box.bottom <= face.box.top) continue;
        int false_face = 0;
        check_rk(rockx_face_filter(filter_, &image, &face.box, &false_face), "RockX face filter");
        if (!false_face) result.boxes.push_back({face.box.left, face.box.top, face.box.right, face.box.bottom, face.score});
    }
    result.completed_at = std::chrono::steady_clock::now();
    return result;
}

RtspServer::RtspServer(int port) {
    server_ = create_rtsp_demo(port);
    if (!server_) throw std::runtime_error("RTSP server creation failed");
    try {
        const char* paths[] = {"/live/main_stream", "/live/sub_stream"};
        for (int i = 0; i < 2; ++i) {
            sessions_[i] = rtsp_new_session(server_, paths[i]);
            if (!sessions_[i]) throw std::runtime_error("RTSP session creation failed");
            check_rk(rtsp_set_video(sessions_[i], RTSP_CODEC_ID_VIDEO_H264, nullptr, 0), "RTSP set video");
            check_rk(rtsp_sync_video_ts(sessions_[i], rtsp_get_reltime(), rtsp_get_ntptime()),
                     "RTSP sync timestamp");
        }
    } catch (...) { rtsp_del_demo(server_); server_ = nullptr; throw; }
}
RtspServer::~RtspServer() { if (server_) rtsp_del_demo(server_); }
void RtspServer::send(int stream, const Frame& packet) {
    const uint64_t pts = packet->pts();
    const size_t size = RK_MPI_MB_GetSize(packet->get());
    if (!size || size > INT_MAX) throw std::runtime_error("invalid encoded packet size");
    CpuRead access(packet->get());
    const auto* data = static_cast<const uint8_t*>(RK_MPI_MB_GetPtr(packet->get()));
    if (!data) throw std::runtime_error("encoded packet mapping failed");
    // rtsp_tx_video may return a non-negative byte count, rather than zero.
    if (rtsp_tx_video(sessions_[stream], data, static_cast<int>(size), pts) < 0)
        throw std::runtime_error("RTSP video transmit failed");
}
void RtspServer::poll() {
    errno = 0;
    if (rtsp_do_event(server_) < 0 && errno != EINTR)
        throw std::runtime_error("RTSP event processing failed");
}
} // namespace vision
