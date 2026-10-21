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
void Isp::disable_tnr() {
#ifdef RKAIQ
    auto* context = static_cast<rk_aiq_sys_ctx_t*>(context_);
    if (!context) return;
    // This old RKAiq generation applies forced ISPP module state while it is
    // processing live parameters. Calling this only after VI has started makes
    // the request reach the hardware instead of being overwritten at startup.
    check_rk(rk_aiq_uapi_sysctl_setModuleCtl(context, RK_MODULE_TNR, false),
             "ISP Disable TNR");
    bool enabled = true;
    check_rk(rk_aiq_uapi_sysctl_getModuleCtl(context, RK_MODULE_TNR, &enabled),
             "ISP Get TNR state");
    if (enabled) throw std::runtime_error("ISP TNR remained enabled");
    std::fprintf(stderr, "ISP TNR forced off\n");
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

HardwareCompositor::HardwareCompositor(int rga_channel, const Config& config)
    : channel_(rga_channel), height_(config.output_height()), enabled_(config.osd) {
    if (enabled_) painter_.reset(new object_detection::LabelPainter(config.labels));
    disable_all();
}
HardwareCompositor::~HardwareCompositor() { disable_all(); }
void HardwareCompositor::disable_slot(int slot) noexcept {
    const int first = slot * 4;
    static std::uint32_t transparent = 0;
    OSD_REGION_INFO_S label{};
    label.enRegionId = static_cast<OSD_REGION_ID_E>(first);
    label.u8Enable = 0;
    BITMAP_S bitmap{};
    bitmap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
    bitmap.u32Width = bitmap.u32Height = 1;
    bitmap.pData = &transparent;
    warn_rk(RK_MPI_RGA_RGN_SetBitMap(channel_, &label, &bitmap),
            "RGA disable label");
    for (int id = first + 1; id < first + 4; ++id) {
        OSD_REGION_INFO_S edge{};
        edge.enRegionId = static_cast<OSD_REGION_ID_E>(id);
        edge.u8Enable = 0;
        warn_rk(RK_MPI_RGA_RGN_SetCover(channel_, &edge, nullptr),
                "RGA disable box edge");
    }
    if (current_labels_[slot])
        retired_labels_[retired_index_++ % retired_labels_.size()] =
            current_labels_[slot];
    current_labels_[slot].reset();
}
void HardwareCompositor::disable_all() noexcept {
    disable_slot(0);
    disable_slot(1);
}
std::size_t HardwareCompositor::update(const std::vector<Box>& boxes) {
    if (!enabled_) return 0;
    const std::size_t count = std::min<std::size_t>(boxes.size(), 2);
    COVER_INFO_S cover{};
    cover.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
    cover.u32Color = 0xff00ff00;
    for (std::size_t slot = 0; slot < count; ++slot) {
        const Box& b = boxes[slot];
        const int box_width = b.right - b.left;
        const int box_height = b.bottom - b.top;
        const int thickness = std::min(4, std::min(box_width, box_height));
        if (box_width <= 0 || box_height <= 0 || thickness <= 0) {
            disable_slot(static_cast<int>(slot));
            continue;
        }
        const bool above = b.top >= 28;
        const int label_y = std::max(0, std::min(above ? b.top - 28 : b.top,
                                                 height_ - 32));
        auto label = std::make_shared<cv::Mat>(
            painter_->render_top_strip(box_width, b, above));
        if (!label->isContinuous() || label->type() != CV_8UC4)
            throw std::runtime_error("Label bitmap is not tightly packed BGRA");
        if (current_labels_[slot])
            retired_labels_[retired_index_++ % retired_labels_.size()] =
                current_labels_[slot];
        current_labels_[slot] = label;

        OSD_REGION_INFO_S region{};
        region.enRegionId = static_cast<OSD_REGION_ID_E>(slot * 4);
        region.u32PosX = static_cast<RK_U32>(b.left);
        region.u32PosY = static_cast<RK_U32>(label_y);
        region.u32Width = static_cast<RK_U32>(box_width);
        region.u32Height = 32;
        region.u8Enable = 1;
        BITMAP_S bitmap{};
        bitmap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
        bitmap.u32Width = region.u32Width;
        bitmap.u32Height = region.u32Height;
        bitmap.pData = label->data;
        check_rk(RK_MPI_RGA_RGN_SetBitMap(channel_, &region, &bitmap),
                 "RGA set label/top edge");

        const int rects[3][4] = {
            {b.left, b.bottom - thickness, box_width, thickness},
            {b.left, b.top, thickness, box_height},
            {b.right - thickness, b.top, thickness, box_height}
        };
        for (int edge_index = 0; edge_index < 3; ++edge_index) {
            const auto& rect = rects[edge_index];
            OSD_REGION_INFO_S edge{};
            edge.enRegionId = static_cast<OSD_REGION_ID_E>(slot * 4 + edge_index + 1);
            edge.u32PosX = static_cast<RK_U32>(rect[0]);
            edge.u32PosY = static_cast<RK_U32>(rect[1]);
            edge.u32Width = static_cast<RK_U32>(rect[2]);
            edge.u32Height = static_cast<RK_U32>(rect[3]);
            edge.u8Enable = 1;
            check_rk(RK_MPI_RGA_RGN_SetCover(channel_, &edge, &cover),
                     "RGA set box edge");
        }
    }
    for (std::size_t slot = count; slot < 2; ++slot)
        disable_slot(static_cast<int>(slot));
    // Retain four previous generations so an in-flight RGA frame cannot see
    // freed label pixels while the region pointer is being replaced.
    return count;
}

ObjectDetector::ObjectDetector(const Config& config)
    : model_(config.model, config.metadata, config.threshold) {}
ObjectDetector::~ObjectDetector() = default;
Detection ObjectDetector::infer(const Frame& frame) {
    const auto info = frame->image_info();
    // This SDK's IMAGE_TYPE_BGR888 means physical RGB, as verified against
    // external/rkmedia/src/rkrga/rga.cc. Do not swap it again for RKNN.
    if (info.enImgType != IMAGE_TYPE_BGR888 || info.u32HorStride < info.u32Width ||
        info.u32VerStride < info.u32Height ||
        RK_MPI_MB_GetSize(frame->get()) < size_t(info.u32HorStride) * info.u32VerStride * 3)
        throw std::runtime_error("Invalid RGB input format/stride");
    CpuRead access(frame->get());
    void* data = RK_MPI_MB_GetPtr(frame->get());
    if (!data) throw std::runtime_error("RKNN input mapping failed");
    cv::Mat image(info.u32Height, info.u32Width, CV_8UC3, data, size_t(info.u32HorStride) * 3);
    Detection result = model_.infer(image);
    result.pts_us = frame->pts();
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
