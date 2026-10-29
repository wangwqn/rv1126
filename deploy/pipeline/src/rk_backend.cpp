#include "vision/rk_backend.hpp"
#include <alsa/asoundlib.h>
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
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

namespace vision {
namespace {
void check_alsa(int status, const std::string& operation) {
    if (status < 0)
        throw std::runtime_error(operation + ": " + snd_strerror(status));
}

void configure_capture_source(const Config& config) {
    if (config.audio_source == "current") return;
    snd_mixer_t* raw_mixer = nullptr;
    check_alsa(snd_mixer_open(&raw_mixer, 0), "ALSA mixer open");
    std::unique_ptr<snd_mixer_t, int (*)(snd_mixer_t*)> mixer(raw_mixer,
                                                              snd_mixer_close);
    check_alsa(snd_mixer_attach(mixer.get(), config.audio_mixer.c_str()),
               "ALSA mixer attach " + config.audio_mixer);
    check_alsa(snd_mixer_selem_register(mixer.get(), nullptr, nullptr),
               "ALSA mixer register");
    check_alsa(snd_mixer_load(mixer.get()), "ALSA mixer load");

    snd_mixer_selem_id_t* raw_id = nullptr;
    check_alsa(snd_mixer_selem_id_malloc(&raw_id), "ALSA mixer id allocation");
    std::unique_ptr<snd_mixer_selem_id_t, void (*)(snd_mixer_selem_id_t*)>
        id(raw_id, snd_mixer_selem_id_free);
    snd_mixer_selem_id_set_index(id.get(), 0);
    snd_mixer_selem_id_set_name(id.get(), "Capture MIC Path");
    snd_mixer_elem_t* element = snd_mixer_find_selem(mixer.get(), id.get());
    if (!element || !snd_mixer_selem_is_enumerated(element))
        throw std::runtime_error("ALSA mixer has no enumerated Capture MIC Path");

    const std::string requested = config.audio_source == "main"
                                      ? "Main Mic"
                                      : "Hands Free Mic";
    const unsigned items = snd_mixer_selem_get_enum_items(element);
    for (unsigned item = 0; item < items; ++item) {
        char name[64]{};
        check_alsa(snd_mixer_selem_get_enum_item_name(element, item,
                                                       sizeof(name), name),
                   "ALSA mixer read capture source");
        if (requested == name) {
            check_alsa(snd_mixer_selem_set_enum_item(
                           element, SND_MIXER_SCHN_FRONT_LEFT, item),
                       "ALSA mixer select " + requested);
            std::fprintf(stderr, "audio: Capture MIC Path=%s\n", requested.c_str());
            return;
        }
    }
    throw std::runtime_error("ALSA mixer does not provide capture source " + requested);
}

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

#ifdef RKAIQ
class FileDescriptor : Noncopyable {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    int get() const { return fd_; }
private:
    int fd_;
};

std::string system_error(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

std::string read_first_line(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return {};
    char line[256]{};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read) return {};
    std::string value(line);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

std::string find_sensor_subdev(const std::string& sensor_name) {
    DIR* raw_dir = ::opendir("/sys/class/video4linux");
    if (!raw_dir) throw std::runtime_error(system_error("open video4linux sysfs"));
    std::unique_ptr<DIR, int (*)(DIR*)> dir(raw_dir, ::closedir);
    while (const dirent* entry = ::readdir(dir.get())) {
        const std::string node(entry->d_name);
        const std::string prefix = "v4l-subdev";
        if (node.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string name = read_first_line("/sys/class/video4linux/" + node + "/name");
        if (name == sensor_name) return "/dev/" + node;
    }
    throw std::runtime_error("cannot find V4L2 subdevice for sensor " + sensor_name);
}

std::string find_video_node(const std::string& entity_name,
                            const std::string& prefix) {
    DIR* raw_dir = ::opendir("/sys/class/video4linux");
    if (!raw_dir) throw std::runtime_error(system_error("open video4linux sysfs"));
    std::unique_ptr<DIR, int (*)(DIR*)> dir(raw_dir, ::closedir);
    while (const dirent* entry = ::readdir(dir.get())) {
        const std::string node(entry->d_name);
        if (node.compare(0, prefix.size(), prefix) != 0) continue;
        if (read_first_line("/sys/class/video4linux/" + node + "/name") == entity_name)
            return "/dev/" + node;
    }
    throw std::runtime_error("cannot find V4L2 node for " + entity_name);
}

media_entity_desc find_media_entity(int fd, const std::string& name) {
    media_entity_desc entity{};
    entity.id = MEDIA_ENT_ID_FLAG_NEXT;
    while (::ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
        if (name == entity.name) return entity;
        entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
    }
    throw std::runtime_error("media entity not found: " + name);
}

std::string find_media_device(const std::string& model,
                              const std::vector<std::string>& entity_names) {
    for (int index = 0; index < 16; ++index) {
        const std::string path = "/dev/media" + std::to_string(index);
        const int raw_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (raw_fd < 0) continue;
        FileDescriptor fd(raw_fd);
        media_device_info info{};
        if (::ioctl(fd.get(), MEDIA_IOC_DEVICE_INFO, &info) < 0 ||
            (!model.empty() && model != info.model))
            continue;
        bool found = true;
        for (const auto& name : entity_names) {
            try { (void)find_media_entity(fd.get(), name); }
            catch (...) { found = false; break; }
        }
        if (found) return path;
    }
    throw std::runtime_error("cannot find media device containing required entities");
}

std::string find_media_entity_node(const std::string& media_device,
                                   const std::string& entity_name,
                                   const std::string& prefix) {
    const int raw_fd = ::open(media_device.c_str(), O_RDWR | O_CLOEXEC);
    if (raw_fd < 0) throw std::runtime_error(system_error("open " + media_device));
    FileDescriptor fd(raw_fd);
    const auto entity = find_media_entity(fd.get(), entity_name);
    const dev_t wanted = makedev(entity.dev.major, entity.dev.minor);
    for (int index = 0; index < 128; ++index) {
        const std::string path = "/dev/" + prefix + std::to_string(index);
        struct stat status{};
        if (::stat(path.c_str(), &status) == 0 && status.st_rdev == wanted)
            return path;
    }
    throw std::runtime_error("cannot resolve device node for " + entity_name +
                             " on " + media_device);
}

struct SavedMediaLink {
    std::string media_device;
    media_link_desc link{};
};

SavedMediaLink set_media_link(const std::string& media_device,
                              const std::string& source_name, unsigned source_pad,
                              const std::string& sink_name, unsigned sink_pad,
                              bool enabled) {
    const int raw_fd = ::open(media_device.c_str(), O_RDWR | O_CLOEXEC);
    if (raw_fd < 0) throw std::runtime_error(system_error("open " + media_device));
    FileDescriptor fd(raw_fd);
    const auto source = find_media_entity(fd.get(), source_name);
    const auto sink = find_media_entity(fd.get(), sink_name);
    std::vector<media_pad_desc> pads(source.pads);
    std::vector<media_link_desc> links(source.links);
    media_links_enum enumeration{};
    enumeration.entity = source.id;
    enumeration.pads = pads.data();
    enumeration.links = links.data();
    if (::ioctl(fd.get(), MEDIA_IOC_ENUM_LINKS, &enumeration) < 0)
        throw std::runtime_error(system_error("enumerate links for " + source_name));
    for (auto link : links) {
        if (link.source.entity != source.id || link.source.index != source_pad ||
            link.sink.entity != sink.id || link.sink.index != sink_pad)
            continue;
        SavedMediaLink saved{media_device, link};
        const std::uint32_t desired = enabled ? (link.flags | MEDIA_LNK_FL_ENABLED)
                                              : (link.flags & ~MEDIA_LNK_FL_ENABLED);
        if (desired != link.flags) {
            if (link.flags & MEDIA_LNK_FL_IMMUTABLE)
                throw std::runtime_error("required media link is immutable");
            link.flags = desired;
            if (::ioctl(fd.get(), MEDIA_IOC_SETUP_LINK, &link) < 0)
                throw std::runtime_error(system_error("configure media link " +
                                                       source_name + " -> " + sink_name));
        }
        return saved;
    }
    throw std::runtime_error("media link not found: " + source_name + " -> " + sink_name);
}

void restore_media_link(const SavedMediaLink& saved) noexcept {
    const int fd = ::open(saved.media_device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    media_link_desc link = saved.link;
    if (::ioctl(fd, MEDIA_IOC_SETUP_LINK, &link) < 0)
        std::fprintf(stderr, "cleanup: restore media link failed: %s\n", std::strerror(errno));
    ::close(fd);
}

struct SavedSubdevFormat {
    std::string node;
    v4l2_subdev_format format{};
};

SavedSubdevFormat set_subdev_format(const std::string& node, unsigned pad,
                                    std::uint32_t code, std::uint32_t width,
                                    std::uint32_t height) {
    const int raw_fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (raw_fd < 0) throw std::runtime_error(system_error("open " + node));
    FileDescriptor fd(raw_fd);
    v4l2_subdev_format current{};
    current.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    current.pad = pad;
    if (::ioctl(fd.get(), VIDIOC_SUBDEV_G_FMT, &current) < 0)
        throw std::runtime_error(system_error("get subdevice format " + node));
    SavedSubdevFormat saved{node, current};
    auto requested = current;
    requested.format.width = width;
    requested.format.height = height;
    requested.format.code = code;
    requested.format.field = V4L2_FIELD_NONE;
    if (::ioctl(fd.get(), VIDIOC_SUBDEV_S_FMT, &requested) < 0)
        throw std::runtime_error(system_error("set subdevice format " + node));
    if (requested.format.width != width || requested.format.height != height ||
        requested.format.code != code)
        throw std::runtime_error("subdevice rejected requested 1080p format: " + node);
    return saved;
}

void restore_subdev_format(const SavedSubdevFormat& saved) noexcept {
    const int fd = ::open(saved.node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    auto format = saved.format;
    if (::ioctl(fd, VIDIOC_SUBDEV_S_FMT, &format) < 0)
        std::fprintf(stderr, "cleanup: restore subdevice format failed: %s\n",
                     std::strerror(errno));
    ::close(fd);
}

void configure_imx415_mode(const std::string& sensor_name, const std::string& mode) {
    if (mode == "current") {
        std::fprintf(stderr, "camera: keep externally configured sensor mode\n");
        return;
    }
    if (sensor_name.find("imx415") == std::string::npos)
        throw std::runtime_error("sensor-mode " + mode + " requires IMX415, found " + sensor_name);

    const bool full_hd = mode == "1080p";
    const std::uint32_t width = full_hd ? 1944u : 3864u;
    const std::uint32_t height = full_hd ? 1097u : 2192u;
    const std::uint32_t code = full_hd ? MEDIA_BUS_FMT_SGBRG12_1X12
                                       : MEDIA_BUS_FMT_SGBRG10_1X10;
    const std::string path = find_sensor_subdev(sensor_name);
    const int raw_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (raw_fd < 0) throw std::runtime_error(system_error("open " + path));
    FileDescriptor fd(raw_fd);

    v4l2_subdev_format format{};
    format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    format.pad = 0;
    if (::ioctl(fd.get(), VIDIOC_SUBDEV_G_FMT, &format) < 0)
        throw std::runtime_error(system_error("get IMX415 format"));
    format.format.width = width;
    format.format.height = height;
    format.format.code = code;
    format.format.field = V4L2_FIELD_NONE;
    if (::ioctl(fd.get(), VIDIOC_SUBDEV_S_FMT, &format) < 0)
        throw std::runtime_error(system_error("set IMX415 " + mode + " format"));
    if (format.format.width != width || format.format.height != height ||
        format.format.code != code) {
        throw std::runtime_error("IMX415 rejected requested " + mode + " sensor mode");
    }

    v4l2_subdev_selection selection{};
    selection.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    selection.pad = 0;
    selection.target = V4L2_SEL_TGT_CROP_BOUNDS;
    if (::ioctl(fd.get(), VIDIOC_SUBDEV_G_SELECTION, &selection) == 0) {
        const std::uint32_t expected_width = full_hd ? 1920u : 3840u;
        const std::uint32_t expected_height = full_hd ? 1080u : 2160u;
        if (selection.r.width != expected_width || selection.r.height != expected_height)
            throw std::runtime_error("unexpected IMX415 crop for " + mode + " sensor mode");
        std::fprintf(stderr,
                     "camera: %s %ux%u RAW%u, active crop %dx%d at (%d,%d)\n",
                     path.c_str(), width, height, full_hd ? 12u : 10u,
                     selection.r.width, selection.r.height,
                     selection.r.left, selection.r.top);
    } else {
        std::fprintf(stderr, "camera: %s %ux%u RAW%u (crop query unavailable)\n",
                     path.c_str(), width, height, full_hd ? 12u : 10u);
    }
}
#endif
} // namespace

class OfflineIsppTopology : Noncopyable {
public:
    OfflineIsppTopology() {
#ifdef RKAIQ
        try {
            const std::string isp_media = find_media_device("rkisp0",
                {"rkisp-isp-subdev", "rkisp-bridge-ispp", "rkisp_mainpath"});
            const std::string ispp_media = find_media_device("rkispp1",
                {"rkispp_input_image", "rkispp-subdev"});

            // ISP20 cannot resize the direct bridge. Route the full-FOV ISP
            // output through mainpath/MRSZ, whose VI format is set to 1080p.
            links_.push_back(set_media_link(isp_media, "rkisp-isp-subdev", 2,
                                            "rkisp-bridge-ispp", 0, false));
            links_.push_back(set_media_link(isp_media, "rkisp-isp-subdev", 2,
                                            "rkisp_mainpath", 0, true));
            links_.push_back(set_media_link(ispp_media, "rkispp_input_image", 0,
                                            "rkispp-subdev", 0, true));

            const std::string ispp_subdev = find_media_entity_node(
                ispp_media, "rkispp-subdev", "v4l-subdev");
            input_node_ = find_media_entity_node(
                ispp_media, "rkispp_input_image", "video");
            main_node_ = find_media_entity_node(
                ispp_media, "rkispp_scale0", "video");
            sub_node_ = find_media_entity_node(
                ispp_media, "rkispp_scale1", "video");
            formats_.push_back(set_subdev_format(ispp_subdev, 0,
                                                  MEDIA_BUS_FMT_YUYV8_2X8,
                                                  1920, 1080));
            formats_.push_back(set_subdev_format(ispp_subdev, 2,
                                                  MEDIA_BUS_FMT_YUYV8_2X8,
                                                  1920, 1080));
            std::fprintf(stderr,
                         "camera: offline ISPP topology enabled: "
                         "RAW10 4K -> ISP mainpath NV12 1080p -> VP DMA-BUF -> "
                         "rkispp1 TNR 1080p; main=%s sub=%s input=%s\n",
                         main_node_.c_str(), sub_node_.c_str(), input_node_.c_str());
        } catch (...) {
            rollback();
            throw;
        }
#else
        throw std::runtime_error("offline ISPP topology requires ISP support");
#endif
    }

    ~OfflineIsppTopology() { rollback(); }

    const std::string& main_node() const { return main_node_; }
    const std::string& sub_node() const { return sub_node_; }
    const std::string& input_node() const { return input_node_; }

private:
    void rollback() noexcept {
#ifdef RKAIQ
        for (auto it = formats_.rbegin(); it != formats_.rend(); ++it)
            restore_subdev_format(*it);
        formats_.clear();
        for (auto it = links_.rbegin(); it != links_.rend(); ++it)
            restore_media_link(*it);
        links_.clear();
#endif
    }

    std::vector<SavedMediaLink> links_;
    std::vector<SavedSubdevFormat> formats_;
    std::string main_node_;
    std::string sub_node_;
    std::string input_node_;
};

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

Isp::Isp(const Config& c) : camera_(c.camera), fps_(c.fps) {
    if (c.iq_dir.empty()) return;
#ifdef RKAIQ
    // RKAiq uses HDR_MODE during initialization even though prepare() also
    // receives the working mode. This process owns a single normal-mode ISP.
    check_rk(::setenv("HDR_MODE", "0", 1), "ISP set HDR_MODE");
    if (c.sensor_mode == "4k-ispp-1080") {
        // The custom library redirects ISP0's dynamic ISPP parameters and
        // statistics to independent rkispp1. Refuse to run this topology with
        // the stock library: it would still control rkispp0 and can wedge this
        // BSP when frames arrive through a DDR input.
        using OverrideVersion = int (*)();
        using AppliedTarget = int (*)();
        auto* version = reinterpret_cast<OverrideVersion>(
            ::dlsym(RTLD_DEFAULT, "rk_aiq_ispp_target_override_version"));
        auto* applied_target = reinterpret_cast<AppliedTarget>(
            ::dlsym(RTLD_DEFAULT, "rk_aiq_ispp_target_override_applied"));
        if (!version || version() < 2 || !applied_target || applied_target() != 1)
            throw std::runtime_error(
                "4k-ispp-1080 requires the candidate launcher to preload "
                "the patched RKAiq library with ISPP target 1");
        std::fprintf(stderr,
                     "camera: RKAiq preload confirmed ISPP target 1 (override ABI v%d)\n",
                     version());
        check_rk(::setenv("RKAIQ_ISPP_TARGET", "1", 1),
                 "ISP set ISPP target");
        check_rk(::setenv("RKAIQ_ISPP_INPUT_SIZE", "1920x1080", 1),
                 "ISP set ISPP input size");
    } else {
        ::unsetenv("RKAIQ_ISPP_TARGET");
        ::unsetenv("RKAIQ_ISPP_INPUT_SIZE");
    }
    rk_aiq_static_info_t info{};
    rk_aiq_uapi_sysctl_enumStaticMetas(camera_, &info);
    const char* sensor_name = info.sensor_info.sensor_name;
    if (!sensor_name || !*sensor_name)
        throw std::runtime_error("ISP camera has no RKAiq static metadata");
    configure_imx415_mode(sensor_name, c.sensor_mode);
    // Select DDR input before RKAiq opens bridge/ISPP parameter streams.
    // Switching a live ISP->ISPP bridge to DDR leaves this BSP generation's
    // internal bridge buffers in the old mode and can wedge on the first frame.
    if (c.sensor_mode == "4k-ispp-1080")
        offline_topology_.reset(new OfflineIsppTopology());
    auto* context = rk_aiq_uapi_sysctl_init(sensor_name,
                                            c.iq_dir.c_str(), nullptr, nullptr);
    if (!context) throw std::runtime_error("ISP Init returned a null RKAiq context");
    context_ = context;
    try {
        check_rk(rk_aiq_uapi_sysctl_prepare(context, 0, 0,
                                            RK_AIQ_WORKING_MODE_NORMAL),
                 "ISP Prepare");
        if (c.sensor_mode == "1080p") {
            // The board's bundled IMX415 IQ file only contains a 3840x2160
            // lens-shading table. Leaving ALSC in automatic mode at 1920x1080
            // makes this old RKAiq release fail the lookup and print an error
            // on every frame. Do not fabricate a calibration table: bypass LSC
            // until a lens-specific 1920x1080 IQ calibration is available.
            // In this BSP rk_aiq_user_api_alsc_SetAttrib() is compiled as a
            // no-op (its setAttrib call is commented out). Disable both the
            // integrated ALSC algorithm and the ISP LSC module through the
            // working sysctl APIs instead.
            check_rk(rk_aiq_uapi_sysctl_enableAxlib(
                         context, RK_AIQ_ALGO_TYPE_ALSC, 0, false),
                     "ISP DisableALSCAlgo");
            check_rk(rk_aiq_uapi_sysctl_setModuleCtl(
                         context, RK_MODULE_LSC, false),
                     "ISP DisableLSCModule");
            std::fprintf(stderr,
                         "camera: ALSC bypassed: IQ file has no calibrated 1920x1080 table\n");
        }
        // The offline DDR input and both ISPP output nodes must be opened
        // before RKAiq starts the ISPP parameter/statistics queues. The direct
        // ISP bridge has a producer during RKAiq start, but an offline VP
        // pipeline does not receive its first buffer until the final binding.
        if (c.sensor_mode != "4k-ispp-1080")
            start();
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

void Isp::start() {
#ifdef RKAIQ
    auto* context = static_cast<rk_aiq_sys_ctx_t*>(context_);
    if (!context || started_) return;
    check_rk(rk_aiq_uapi_sysctl_start(context), "ISP Start");
    started_ = true;
    try {
        frameRateInfo_t rate{};
        rate.mode = OP_MANUAL;
        rate.fps = fps_;
        check_rk(rk_aiq_uapi_setFrameRate(context, rate), "ISP SetFrameRate");
    } catch (...) {
        rk_aiq_uapi_sysctl_stop(context, false);
        started_ = false;
        throw;
    }
#endif
}

const std::string& Isp::offline_main_node() const {
    if (!offline_topology_)
        throw std::logic_error("offline ISPP topology is not active");
    return offline_topology_->main_node();
}

const std::string& Isp::offline_sub_node() const {
    if (!offline_topology_)
        throw std::logic_error("offline ISPP topology is not active");
    return offline_topology_->sub_node();
}

const std::string& Isp::offline_input_node() const {
    if (!offline_topology_)
        throw std::logic_error("offline ISPP topology is not active");
    return offline_topology_->input_node();
}
Isp::~Isp() {
#ifdef RKAIQ
    auto* context = static_cast<rk_aiq_sys_ctx_t*>(context_);
    if (context) {
        if (started_) rk_aiq_uapi_sysctl_stop(context, false);
        rk_aiq_uapi_sysctl_deinit(context);
    }
    // Media nodes and RKAiq parameter streams are now closed. Restore the
    // normal direct bridge so a later legacy launch starts from a clean graph.
    offline_topology_.reset();
#endif
}

ViChannel::ViChannel(int camera, int channel, std::string node, int width, int height, int buffers)
    : camera_(camera), channel_(channel), node_(std::move(node)) {
#ifdef RKAIQ
    // RKMedia's old entity-name resolver knows rkispp_scale*, but not all ISP
    // nodes (notably rkisp_mainpath). Resolve unsupported names ourselves.
    if (!node_.empty() && node_[0] != '/' && node_.compare(0, 6, "rkisp_") == 0)
        node_ = find_video_node(node_, "video");
#endif
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

VpChannel::VpChannel(int pipe, int channel, std::string node, int width,
                     int height, int buffers)
    : pipe_(pipe), channel_(channel), node_(std::move(node)) {
#ifdef RKAIQ
    if (!node_.empty() && node_[0] != '/')
        node_ = find_video_node(node_, "video");
#endif
    VP_CHN_ATTR_S attr{};
    attr.pcVideoNode = const_cast<char*>(node_.c_str());
    attr.u32BufCnt = buffers;
    attr.u32Width = width;
    attr.u32Height = height;
    attr.enPixFmt = IMAGE_TYPE_NV12;
    attr.enBufType = VP_CHN_BUF_TYPE_DMA;
    attr.enWorkMode = VP_WORK_MODE_NORMAL;
    check_rk(RK_MPI_VP_SetChnAttr(pipe_, channel_, &attr), "VP SetChnAttr");
    try {
        check_rk(RK_MPI_VP_EnableChn(pipe_, channel_), "VP EnableChn");
    } catch (...) {
        warn_rk(RK_MPI_VP_DisableChn(pipe_, channel_), "VP rollback DisableChn");
        throw;
    }
}
VpChannel::~VpChannel() {
    warn_rk(RK_MPI_VP_DisableChn(pipe_, channel_), "VP DisableChn");
}
MPP_CHN_S VpChannel::endpoint() const { return {RK_ID_VP, pipe_, channel_}; }

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
void RgaChannel::limit_input_rate(int input_fps, int output_fps) {
    MPP_FPS_ATTR_S rate{};
    rate.s32FpsInNum = input_fps;
    rate.s32FpsInDen = 1;
    rate.s32FpsOutNum = output_fps;
    rate.s32FpsOutDen = 1;
    check_rk(RK_MPI_SYS_SetFrameRate(RK_ID_RGA, channel_, &rate),
             "RGA SetFrameRate");
}
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
    : channel_(rga_channel), height_(config.output_height()),
      max_boxes_(config.max_osd_boxes), enabled_(config.osd) {
    if (enabled_) painter_.reset(new object_detection::LabelPainter(config.labels));
    disable_all();
}
HardwareCompositor::~HardwareCompositor() { disable_all(); }
void HardwareCompositor::disable_slot(int slot) noexcept {
    static std::uint32_t transparent = 0;
    OSD_REGION_INFO_S region{};
    region.enRegionId = static_cast<OSD_REGION_ID_E>(slot);
    region.u8Enable = 0;
    BITMAP_S bitmap{};
    bitmap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
    bitmap.u32Width = bitmap.u32Height = 1;
    bitmap.pData = &transparent;
    warn_rk(RK_MPI_RGA_RGN_SetBitMap(channel_, &region, &bitmap),
            "RGA disable box overlay");
    if (current_overlays_[slot])
        retired_overlays_[retired_index_++ % retired_overlays_.size()] =
            current_overlays_[slot];
    current_overlays_[slot].reset();
}
void HardwareCompositor::disable_all() noexcept {
    for (std::size_t slot = 0; slot < region_count_; ++slot)
        disable_slot(static_cast<int>(slot));
}
std::size_t HardwareCompositor::update(const std::vector<Box>& boxes) {
    if (!enabled_) return 0;
    const std::size_t count = std::min<std::size_t>(
        boxes.size(), static_cast<std::size_t>(max_boxes_));
    for (std::size_t slot = 0; slot < count; ++slot) {
        const Box& b = boxes[slot];
        const int box_width = b.right - b.left;
        const int box_height = b.bottom - b.top;
        if (box_width <= 0 || box_height <= 0) {
            disable_slot(static_cast<int>(slot));
            continue;
        }
        const bool above = b.top >= 32;
        auto overlay = std::make_shared<cv::Mat>(
            painter_->render_box_overlay(box_width, box_height, b, above));
        if (!overlay->isContinuous() || overlay->type() != CV_8UC4 ||
            overlay->rows > height_)
            throw std::runtime_error("Box overlay is not a valid packed BGRA bitmap");
        if (current_overlays_[slot])
            retired_overlays_[retired_index_++ % retired_overlays_.size()] =
                current_overlays_[slot];
        current_overlays_[slot] = overlay;

        OSD_REGION_INFO_S region{};
        region.enRegionId = static_cast<OSD_REGION_ID_E>(slot);
        region.u32PosX = static_cast<RK_U32>(b.left);
        region.u32PosY = static_cast<RK_U32>(above ? b.top - 32 : b.top);
        region.u32Width = static_cast<RK_U32>(overlay->cols);
        region.u32Height = static_cast<RK_U32>(overlay->rows);
        region.u8Enable = 1;
        BITMAP_S bitmap{};
        bitmap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
        bitmap.u32Width = region.u32Width;
        bitmap.u32Height = region.u32Height;
        bitmap.pData = overlay->data;
        check_rk(RK_MPI_RGA_RGN_SetBitMap(channel_, &region, &bitmap),
                 "RGA set box/label overlay");
    }
    for (std::size_t slot = count; slot < region_count_; ++slot)
        disable_slot(static_cast<int>(slot));
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

AlsaCapture::AlsaCapture(const Config& config)
    : channels_(static_cast<unsigned>(config.audio_input_channels)),
      frames_per_packet_(sample_rate * static_cast<unsigned>(config.audio_period_ms) /
                         1000u) {
    configure_capture_source(config);
    snd_pcm_t* handle = nullptr;
    check_alsa(snd_pcm_open(&handle, config.audio_device.c_str(),
                            SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK),
               "ALSA capture open " + config.audio_device);
    try {
        check_alsa(snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE,
                                      SND_PCM_ACCESS_RW_INTERLEAVED,
                                      channels_, sample_rate, 1, 40000),
                   "ALSA capture configure");
        // A non-blocking capture handle remains PREPARED after set_params();
        // unlike a blocking read it does not implicitly start before EAGAIN.
        check_alsa(snd_pcm_start(handle), "ALSA capture start");
        pcm_ = handle;
    } catch (...) {
        snd_pcm_close(handle);
        throw;
    }
    std::fprintf(stderr,
                 "audio: ALSA %s PCM S16LE %u Hz %u channel(s), %zu frames/packet\n",
                 config.audio_device.c_str(), sample_rate, channels_,
                 frames_per_packet_);
}

AlsaCapture::~AlsaCapture() {
    auto* handle = static_cast<snd_pcm_t*>(pcm_);
    if (!handle) return;
    snd_pcm_drop(handle);
    snd_pcm_close(handle);
}

std::size_t AlsaCapture::read_frames(std::int16_t* interleaved,
                                     std::size_t frames, int timeout_ms) {
    if (!interleaved || !frames) return 0;
    auto* handle = static_cast<snd_pcm_t*>(pcm_);
    snd_pcm_sframes_t result = snd_pcm_readi(handle, interleaved, frames);
    if (result == -EAGAIN) {
        const int ready = snd_pcm_wait(handle, timeout_ms);
        if (ready == 0 || ready == -EINTR) return 0;
        if (ready < 0) result = ready;
        else result = snd_pcm_readi(handle, interleaved, frames);
    }
    if (result == -EINTR || result == -EAGAIN) return 0;
    if (result < 0) {
        const int recovered = snd_pcm_recover(handle, static_cast<int>(result), 1);
        if (recovered < 0)
            throw std::runtime_error(std::string("ALSA capture read: ") +
                                     snd_strerror(recovered));
        if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED)
            check_alsa(snd_pcm_start(handle), "ALSA capture restart");
        ++recoveries_;
        return 0;
    }
    return static_cast<std::size_t>(result);
}

} // namespace vision
