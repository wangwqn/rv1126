#include "vision/rk_backend.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vision {
namespace {

// Absorb the board firmware's sub-second Wi-Fi power-state transition without
// dropping a GOP. Longer congestion still enters the IDR resync path so live
// latency cannot grow without bound.
constexpr std::size_t kMaxInputEvents = 512;
constexpr std::size_t kRtpPayload = 1400;

void put_u16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 8);
    output[1] = static_cast<std::uint8_t>(value);
}

void put_u32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

std::string lower(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::uint32_t rtp_timestamp(std::uint64_t pts_us, std::uint32_t clock_rate) {
    return static_cast<std::uint32_t>((pts_us * clock_rate) / 1000000ULL);
}

bool rtp_timestamp_after(std::uint32_t current, std::uint32_t previous) {
    // RTP timestamps wrap modulo 2^32. A signed delta implements the ordering
    // from RFC 3550 as long as samples are less than half a timestamp cycle
    // apart, which is always true for adjacent live video frames.
    return static_cast<std::int32_t>(current - previous) > 0;
}

std::uint32_t next_identifier() {
    static std::atomic<std::uint32_t> value{0x41c64e6dU};
    return value.fetch_add(0x9e3779b9U, std::memory_order_relaxed);
}

struct NalRange {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

std::vector<NalRange> annexb_nals(const std::uint8_t* data, std::size_t size) {
    std::vector<NalRange> result;
    if (!data || !size) return result;

    struct Start { std::size_t prefix; std::size_t header; };
    std::vector<Start> starts;
    for (std::size_t offset = 0; offset + 3 < size;) {
        if (data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 1) {
            starts.push_back({offset, offset + 3});
            offset += 3;
        } else if (offset + 4 < size && data[offset] == 0 &&
                   data[offset + 1] == 0 && data[offset + 2] == 0 &&
                   data[offset + 3] == 1) {
            starts.push_back({offset, offset + 4});
            offset += 4;
        } else {
            ++offset;
        }
    }
    if (starts.empty()) {
        result.push_back({data, size});
        return result;
    }
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const auto begin = starts[index].header;
        auto end = index + 1 < starts.size() ? starts[index + 1].prefix : size;
        while (end > begin && data[end - 1] == 0) --end;
        if (end > begin) result.push_back({data + begin, end - begin});
    }
    return result;
}

class CpuEncodedRead {
public:
    explicit CpuEncodedRead(MEDIA_BUFFER buffer) : buffer_(buffer) {
        check_rk(RK_MPI_MB_BeginCPUAccess(buffer_, RK_TRUE),
                 "encoded MB BeginCPUAccess");
    }
    ~CpuEncodedRead() {
        const int status = RK_MPI_MB_EndCPUAccess(buffer_, RK_TRUE);
        if (status)
            std::fprintf(stderr, "rtsp: encoded MB EndCPUAccess failed (%d)\n",
                         status);
    }
private:
    MEDIA_BUFFER buffer_;
};

}  // namespace

struct RtspServer::Impl {
    struct MediaEvent {
        enum class Kind { video, audio } kind = Kind::video;
        int stream = 0;
        Frame frame;
        std::vector<std::uint8_t> audio;
        std::uint64_t pts_us = 0;
    };

    struct Client {
        struct OutputPacket {
            std::vector<std::uint8_t> data;
            bool media = false;
        };
        int fd = -1;
        int stream = -1;
        bool video_setup = false;
        bool audio_setup = false;
        bool playing = false;
        bool waiting_for_idr = true;
        bool backpressured = false;
        bool close_after_flush = false;
        std::uint8_t video_channel = 0;
        std::uint8_t audio_channel = 2;
        std::uint16_t video_sequence = 0;
        std::uint16_t audio_sequence = 0;
        std::uint32_t video_ssrc = 0;
        std::uint32_t audio_ssrc = 0;
        std::uint32_t last_video_timestamp = 0;
        bool video_timestamp_valid = false;
        std::uint64_t nonmonotonic_video_drops = 0;
        std::string session;
        std::string input;
        std::deque<OutputPacket> output;
        std::size_t output_offset = 0;
        std::size_t output_bytes = 0;
        std::size_t output_high_water = 0;
        std::uint64_t backpressure_recoveries = 0;
        bool failed = false;
    };

    Impl(int port, bool audio, std::size_t max_client_output)
        : port_(port), audio_(audio), max_client_output_(max_client_output) {
        if (max_client_output_ < 128 * 1024 ||
            max_client_output_ > 4 * 1024 * 1024)
            throw std::invalid_argument("invalid RTSP client output limit");
        try {
            listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd_ < 0) fail("RTSP socket");
            set_nonblocking(listen_fd_);
            int reuse = 1;
            if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                             sizeof(reuse)) < 0)
                fail("RTSP SO_REUSEADDR");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(static_cast<std::uint16_t>(port_));
            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                       sizeof(address)) < 0)
                fail("RTSP bind");
            if (::listen(listen_fd_, 16) < 0) fail("RTSP listen");
            if (::pipe(wake_) < 0) fail("RTSP wake pipe");
            set_nonblocking(wake_[0]);
            set_nonblocking(wake_[1]);
            worker_ = std::thread(&Impl::run, this);
        } catch (...) {
            close_fd(wake_[0]);
            close_fd(wake_[1]);
            close_fd(listen_fd_);
            throw;
        }
        std::fprintf(stderr,
                     "rtsp: built-in async RTP/TCP server listening on port %d%s "
                     "client_buffer=%zu bytes\n",
                     port_, audio_ ? " with G.711A audio" : "",
                     max_client_output_);
    }

    ~Impl() {
        stopping_.store(true);
        wake();
        if (worker_.joinable()) worker_.join();
        close_fd(wake_[0]);
        close_fd(wake_[1]);
        close_fd(listen_fd_);
    }

    void push_video(int stream, const Frame& frame) {
        if (stream < 0 || stream > 1 || !frame)
            throw std::runtime_error("invalid RTSP video packet");
        if (players_.load(std::memory_order_relaxed) == 0) return;
        MediaEvent event;
        event.kind = MediaEvent::Kind::video;
        event.stream = stream;
        event.frame = frame;
        event.pts_us = frame->pts();
        push(std::move(event));
    }

    void push_audio(const std::vector<std::uint8_t>& data,
                    std::uint64_t pts_us) {
        if (!audio_) return;
        if (data.empty()) throw std::runtime_error("invalid RTSP audio packet");
        if (players_.load(std::memory_order_relaxed) == 0) return;
        MediaEvent event;
        event.kind = MediaEvent::Kind::audio;
        event.audio = data;
        event.pts_us = pts_us;
        push(std::move(event));
    }

private:
    static void close_fd(int& fd) noexcept {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    [[noreturn]] void fail(const char* operation) {
        const int saved = errno;
        close_fd(wake_[0]);
        close_fd(wake_[1]);
        close_fd(listen_fd_);
        throw std::runtime_error(std::string(operation) + ": " +
                                 std::strerror(saved));
    }

    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::runtime_error(std::string("fcntl nonblocking: ") +
                                     std::strerror(errno));
        const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
        if (descriptor_flags >= 0)
            static_cast<void>(::fcntl(fd, F_SETFD,
                                      descriptor_flags | FD_CLOEXEC));
    }

    void wake() noexcept {
        const std::uint8_t byte = 1;
        static_cast<void>(::write(wake_[1], &byte, sizeof(byte)));
    }

    void push(MediaEvent event) {
        bool notify = false;
        MediaEvent retired;
        std::size_t high_water = 0;
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            notify = events_.empty();
            if (events_.size() == kMaxInputEvents) {
                retired = std::move(events_.front());
                events_.pop_front();
                ++input_drops_;
            }
            events_.push_back(std::move(event));
            if (events_.size() >= events_high_water_ + 64) {
                events_high_water_ = events_.size();
                high_water = events_high_water_;
            }
        }
        if (high_water)
            std::fprintf(stderr,
                         "rtsp: media input queue high-water=%zu events\n",
                         high_water);
        if (notify) wake();
    }

    void run() noexcept {
        while (!stopping_.load()) {
            // The pollfd array describes this snapshot. Accept new clients
            // only after processing it, otherwise clients_ can outgrow the
            // descriptor array during this iteration.
            const std::size_t polled_clients = clients_.size();
            std::vector<pollfd> descriptors;
            descriptors.reserve(polled_clients + 2);
            descriptors.push_back({listen_fd_, POLLIN, 0});
            descriptors.push_back({wake_[0], POLLIN, 0});
            for (const auto& client : clients_) {
                short events = POLLIN;
                if (!client->output.empty() || client->backpressured)
                    events |= POLLOUT;
                descriptors.push_back({client->fd, events, 0});
            }
            const int ready = ::poll(descriptors.data(), descriptors.size(), 100);
            if (ready < 0 && errno != EINTR) {
                std::fprintf(stderr, "rtsp: poll failed: %s\n", std::strerror(errno));
                break;
            }
            if (descriptors[1].revents & POLLIN) drain_wake();
            for (std::size_t index = 0; index < polled_clients; ++index) {
                Client& client = *clients_[index];
                const short revents = descriptors[index + 2].revents;
                if (revents & (POLLERR | POLLHUP | POLLNVAL)) client.failed = true;
                if (!client.failed && (revents & POLLIN)) read_client(client);
                if (!client.failed && (revents & POLLOUT)) write_client(client);
            }
            if (descriptors[0].revents & POLLIN) accept_clients();
            drain_media();
            for (auto it = clients_.begin(); it != clients_.end();) {
                Client& client = **it;
                if (client.close_after_flush && client.output.empty())
                    client.failed = true;
                if (!client.failed) {
                    ++it;
                    continue;
                }
                if (client.playing) --players_;
                close_fd(client.fd);
                it = clients_.erase(it);
            }
        }
        for (auto& client : clients_) close_fd(client->fd);
        clients_.clear();
        players_.store(0);
    }

    void accept_clients() noexcept {
        while (true) {
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                              &length);
            if (fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    std::fprintf(stderr, "rtsp: accept failed: %s\n",
                                 std::strerror(errno));
                return;
            }
            try {
                set_nonblocking(fd);
                int enabled = 1;
                static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                                               &enabled, sizeof(enabled)));
                static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                                               &enabled, sizeof(enabled)));
                // AF41 maps to the Wi-Fi WMM video access category on the
                // Broadcom path, reducing the chance that a main-stream TCP
                // loss collapses its congestion window behind best-effort
                // traffic. SO_PRIORITY is best-effort and succeeds as root.
                int video_tos = 0x88;
                static_cast<void>(::setsockopt(fd, IPPROTO_IP, IP_TOS,
                                               &video_tos,
                                               sizeof(video_tos)));
                int socket_priority = 5;
                static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_PRIORITY,
                                               &socket_priority,
                                               sizeof(socket_priority)));
                // Bound kernel buffering as well as the application queue.
                // A large SO_SNDBUF lets an IDR burst monopolize the Windows
                // hotspot path and hides seconds of latency during congestion.
                // Linux doubles this request internally, so 64 KiB provides
                // roughly 128 KiB of smoothing while preserving backpressure.
                int send_buffer = 64 * 1024;
                static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                                               &send_buffer,
                                               sizeof(send_buffer)));
                auto client = std::unique_ptr<Client>(new Client);
                client->fd = fd;
                client->video_sequence =
                    static_cast<std::uint16_t>(next_identifier());
                client->audio_sequence =
                    static_cast<std::uint16_t>(next_identifier());
                client->video_ssrc = next_identifier();
                client->audio_ssrc = next_identifier();
                std::ostringstream session;
                session << std::hex << next_identifier();
                client->session = session.str();
                clients_.push_back(std::move(client));
            } catch (const std::exception& error) {
                std::fprintf(stderr, "rtsp: client setup failed: %s\n", error.what());
                ::close(fd);
            }
        }
    }

    void drain_wake() noexcept {
        std::uint8_t bytes[64];
        while (::read(wake_[0], bytes, sizeof(bytes)) > 0) {}
    }

    void read_client(Client& client) noexcept {
        char bytes[8192];
        while (true) {
            const ssize_t count = ::recv(client.fd, bytes, sizeof(bytes), 0);
            if (count > 0) {
                client.input.append(bytes, static_cast<std::size_t>(count));
                if (client.input.size() > 128 * 1024) {
                    client.failed = true;
                    return;
                }
                continue;
            }
            if (count == 0) client.failed = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                client.failed = true;
            break;
        }
        while (!client.failed && !client.input.empty()) {
            if (static_cast<unsigned char>(client.input[0]) == '$') {
                if (client.input.size() < 4) return;
                const std::size_t length =
                    (static_cast<unsigned char>(client.input[2]) << 8) |
                    static_cast<unsigned char>(client.input[3]);
                if (client.input.size() < length + 4) return;
                client.input.erase(0, length + 4);  // inbound RTCP
                continue;
            }
            const auto header_end = client.input.find("\r\n\r\n");
            if (header_end == std::string::npos) return;
            std::size_t total = header_end + 4;
            const std::string headers = client.input.substr(0, header_end + 2);
            const auto content = lower(headers).find("content-length:");
            if (content != std::string::npos) {
                const auto value = content + std::strlen("content-length:");
                total += static_cast<std::size_t>(
                    std::strtoul(headers.c_str() + value, nullptr, 10));
            }
            if (client.input.size() < total) return;
            const std::string request = client.input.substr(0, total);
            client.input.erase(0, total);
            handle_request(client, request);
        }
    }

    std::map<std::string, std::string> headers(const std::string& request) {
        std::map<std::string, std::string> result;
        std::istringstream input(request);
        std::string line;
        std::getline(input, line);  // request line
        while (std::getline(input, line)) {
            if (line == "\r" || line.empty()) break;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            result[lower(trim(line.substr(0, colon)))] =
                trim(line.substr(colon + 1));
        }
        return result;
    }

    int target_stream(const std::string& target) const {
        if (target.find("/live/main_stream") != std::string::npos) return 0;
        if (target.find("/live/sub_stream") != std::string::npos) return 1;
        return -1;
    }

    void recover_backpressure(Client& client) noexcept {
        std::size_t retained = 0;
        std::deque<Client::OutputPacket> output;
        for (std::size_t index = 0; index < client.output.size(); ++index) {
            auto& packet = client.output[index];
            // A partially written TCP record must be completed; dropping its
            // tail would permanently desynchronize RTSP interleaved framing.
            const bool partial_front = index == 0 && client.output_offset != 0;
            if (packet.media && !partial_front) continue;
            retained += packet.data.size();
            if (partial_front) retained -= client.output_offset;
            output.push_back(std::move(packet));
        }
        client.output.swap(output);
        client.output_bytes = retained;
        client.waiting_for_idr = true;
        client.backpressured = true;
        ++client.backpressure_recoveries;
        std::fprintf(stderr,
                     "rtsp: stream=%d backpressure recovery=%llu retained=%zu bytes\n",
                     client.stream,
                     static_cast<unsigned long long>(
                         client.backpressure_recoveries),
                     retained);
    }

    void enqueue(Client& client, std::vector<std::uint8_t> packet,
                 bool media = false) noexcept {
        if (client.failed) return;
        if (client.output_bytes + packet.size() > max_client_output_) {
            if (!media) {
                std::fprintf(stderr,
                             "rtsp: disconnecting unresponsive control client "
                             "(queued=%zu bytes)\n",
                             client.output_bytes);
                client.failed = true;
                return;
            }
            recover_backpressure(client);
            // The current access unit predates the new IDR request. Drop it;
            // video resumes from the next independently decodable frame.
            return;
        }
        client.output_bytes += packet.size();
        client.output.push_back({std::move(packet), media});
        if (client.output_bytes >= client.output_high_water + 256 * 1024) {
            client.output_high_water = client.output_bytes;
            std::fprintf(stderr,
                         "rtsp: stream=%d output queue high-water=%zu bytes\n",
                         client.stream, client.output_high_water);
        }
    }

    void enqueue(Client& client, const std::string& response) noexcept {
        enqueue(client, std::vector<std::uint8_t>(response.begin(), response.end()),
                false);
    }

    void respond(Client& client, const std::string& cseq, int code,
                 const char* reason, const std::string& extra = {},
                 const std::string& body = {}) noexcept {
        std::ostringstream response;
        response << "RTSP/1.0 " << code << ' ' << reason << "\r\n"
                 << "CSeq: " << (cseq.empty() ? "0" : cseq) << "\r\n"
                 << "Server: rv1126-async-rtp/1.0\r\n" << extra;
        if (!body.empty())
            response << "Content-Type: application/sdp\r\n"
                     << "Content-Length: " << body.size() << "\r\n";
        response << "\r\n" << body;
        enqueue(client, response.str());
    }

    void handle_request(Client& client, const std::string& request) noexcept {
        std::istringstream first(request.substr(0, request.find("\r\n")));
        std::string method, target, protocol;
        first >> method >> target >> protocol;
        const auto values = headers(request);
        const auto cseq_it = values.find("cseq");
        const std::string cseq = cseq_it == values.end() ? std::string{} :
                                                              cseq_it->second;
        if (method.empty() || target.empty() || protocol.find("RTSP/") != 0 ||
            cseq.empty()) {
            respond(client, cseq, 400, "Bad Request");
            return;
        }
        if (method == "OPTIONS") {
            respond(client, cseq, 200, "OK",
                    "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, "
                    "TEARDOWN, GET_PARAMETER\r\n");
            return;
        }
        if (method == "DESCRIBE") {
            const int stream = target_stream(target);
            if (stream < 0) {
                respond(client, cseq, 404, "Not Found");
                return;
            }
            client.stream = stream;
            std::string base = target;
            if (base.back() != '/') base.push_back('/');
            std::ostringstream sdp;
            sdp << "v=0\r\n"
                << "o=- " << next_identifier() << " 1 IN IP4 0.0.0.0\r\n"
                << "s=RV1126 " << (stream ? "sub" : "main") << " stream\r\n"
                << "t=0 0\r\n"
                << "a=control:*\r\n"
                << "a=sendonly\r\n"
                << "m=video 0 RTP/AVP 96\r\n"
                << "a=rtpmap:96 H264/90000\r\n"
                << "a=fmtp:96 packetization-mode=1\r\n"
                << "a=control:trackID=0\r\n";
            if (audio_)
                sdp << "m=audio 0 RTP/AVP 8\r\n"
                    << "a=rtpmap:8 PCMA/8000/1\r\n"
                    << "a=control:trackID=1\r\n";
            respond(client, cseq, 200, "OK", "Content-Base: " + base + "\r\n",
                    sdp.str());
            return;
        }
        if (method == "SETUP") {
            const int stream = target_stream(target);
            if (stream < 0 || (client.stream >= 0 && client.stream != stream)) {
                respond(client, cseq, 404, "Not Found");
                return;
            }
            const auto transport_it = values.find("transport");
            if (transport_it == values.end() ||
                lower(transport_it->second).find("rtp/avp/tcp") ==
                    std::string::npos) {
                respond(client, cseq, 461, "Unsupported Transport");
                return;
            }
            client.stream = stream;
            const bool audio_track = target.find("trackID=1") != std::string::npos;
            unsigned first_channel = audio_track ? 2U : 0U;
            unsigned second_channel = first_channel + 1;
            const std::string transport_lower = lower(transport_it->second);
            const auto position = transport_lower.find("interleaved=");
            if (position != std::string::npos) {
                const char* channels = transport_lower.c_str() + position + 12;
                if (std::sscanf(channels, "%u-%u", &first_channel,
                                &second_channel) != 2 || first_channel > 254 ||
                    second_channel > 255) {
                    respond(client, cseq, 461, "Unsupported Transport");
                    return;
                }
            }
            if (audio_track) {
                if (!audio_) {
                    respond(client, cseq, 404, "Not Found");
                    return;
                }
                client.audio_setup = true;
                client.audio_channel = static_cast<std::uint8_t>(first_channel);
            } else {
                client.video_setup = true;
                client.video_channel = static_cast<std::uint8_t>(first_channel);
            }
            std::ostringstream extra;
            extra << "Transport: RTP/AVP/TCP;unicast;interleaved="
                  << first_channel << '-' << second_channel << "\r\n"
                  << "Session: " << client.session << ";timeout=60\r\n";
            respond(client, cseq, 200, "OK", extra.str());
            return;
        }
        if (method == "PLAY") {
            if (client.stream < 0 || !client.video_setup) {
                respond(client, cseq, 455, "Method Not Valid in This State");
                return;
            }
            if (!client.playing) {
                client.playing = true;
                client.waiting_for_idr = true;
                ++players_;
            }
            static_cast<void>(RK_MPI_VENC_RequestIDR(client.stream, RK_TRUE));
            respond(client, cseq, 200, "OK",
                    "Session: " + client.session + "\r\nRange: npt=0.000-\r\n");
            return;
        }
        if (method == "PAUSE") {
            if (client.playing) {
                client.playing = false;
                --players_;
            }
            respond(client, cseq, 200, "OK",
                    "Session: " + client.session + "\r\n");
            return;
        }
        if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
            respond(client, cseq, 200, "OK",
                    "Session: " + client.session + "\r\n");
            return;
        }
        if (method == "TEARDOWN") {
            respond(client, cseq, 200, "OK",
                    "Session: " + client.session + "\r\n");
            client.close_after_flush = true;
            return;
        }
        respond(client, cseq, 405, "Method Not Allowed");
    }

    void write_client(Client& client) noexcept {
        while (!client.output.empty()) {
            auto& packet = client.output.front().data;
            const std::uint8_t* data = packet.data() + client.output_offset;
            const std::size_t remaining = packet.size() - client.output_offset;
            const ssize_t count = ::send(client.fd, data, remaining, MSG_NOSIGNAL);
            if (count > 0) {
                client.output_offset += static_cast<std::size_t>(count);
                client.output_bytes -= static_cast<std::size_t>(count);
                if (client.output_offset == packet.size()) {
                    client.output.pop_front();
                    client.output_offset = 0;
                }
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                              errno == EINTR))
                return;
            client.failed = true;
            return;
        }
        if (client.backpressured) {
            client.backpressured = false;
            client.waiting_for_idr = true;
            if (client.stream >= 0)
                static_cast<void>(RK_MPI_VENC_RequestIDR(client.stream,
                                                         RK_TRUE));
            std::fprintf(stderr,
                         "rtsp: stream=%d transport writable; requesting IDR\n",
                         client.stream);
        }
    }

    void append_rtp_packet(std::vector<std::uint8_t>& packet,
                           std::uint8_t channel, std::uint8_t payload_type,
                           bool marker, std::uint16_t sequence,
                           std::uint32_t timestamp, std::uint32_t ssrc,
                           const std::uint8_t* first, std::size_t first_size,
                           const std::uint8_t* second = nullptr,
                           std::size_t second_size = 0) {
        const std::size_t rtp_size = 12 + first_size + second_size;
        const std::size_t begin = packet.size();
        packet.resize(begin + 4 + rtp_size);
        std::uint8_t* output = packet.data() + begin;
        output[0] = '$';
        output[1] = channel;
        put_u16(output + 2, static_cast<std::uint16_t>(rtp_size));
        output[4] = 0x80;
        output[5] = static_cast<std::uint8_t>(payload_type |
                                              (marker ? 0x80 : 0));
        put_u16(output + 6, sequence);
        put_u32(output + 8, timestamp);
        put_u32(output + 12, ssrc);
        if (first_size) std::memcpy(output + 16, first, first_size);
        if (second_size)
            std::memcpy(output + 16 + first_size, second, second_size);
    }

    std::vector<std::uint8_t> rtp_packet(std::uint8_t channel,
                                         std::uint8_t payload_type, bool marker,
                                         std::uint16_t sequence,
                                         std::uint32_t timestamp,
                                         std::uint32_t ssrc,
                                         const std::uint8_t* first,
                                         std::size_t first_size) {
        std::vector<std::uint8_t> packet;
        packet.reserve(16 + first_size);
        append_rtp_packet(packet, channel, payload_type, marker, sequence,
                          timestamp, ssrc, first, first_size);
        return packet;
    }

    void packetize_video(Client& client, const std::vector<NalRange>& nals,
                         std::uint32_t timestamp) noexcept {
        std::size_t payload_bytes = 0;
        for (const auto& nal : nals) payload_bytes += nal.size;
        std::vector<std::uint8_t> access_unit;
        // One allocation and one output-queue node per encoded frame. The
        // vector still contains valid RTSP interleaved framing for every RTP
        // packet, so partial nonblocking writes remain safe.
        access_unit.reserve(payload_bytes +
                            ((payload_bytes / (kRtpPayload - 2)) + 8) * 16);
        for (std::size_t nal_index = 0; nal_index < nals.size(); ++nal_index) {
            const auto& nal = nals[nal_index];
            const bool last_nal = nal_index + 1 == nals.size();
            if (!nal.size) continue;
            if (nal.size <= kRtpPayload) {
                append_rtp_packet(access_unit, client.video_channel, 96,
                                  last_nal, client.video_sequence++, timestamp,
                                  client.video_ssrc, nal.data, nal.size);
                continue;
            }
            const std::uint8_t indicator =
                static_cast<std::uint8_t>((nal.data[0] & 0xe0U) | 28U);
            const std::uint8_t type = nal.data[0] & 0x1fU;
            std::size_t offset = 1;
            bool first = true;
            while (offset < nal.size) {
                const std::size_t count =
                    std::min(kRtpPayload - 2, nal.size - offset);
                const bool last_fragment = offset + count == nal.size;
                const std::uint8_t header = static_cast<std::uint8_t>(
                    type | (first ? 0x80U : 0U) |
                    (last_fragment ? 0x40U : 0U));
                const std::uint8_t prefix[] = {indicator, header};
                append_rtp_packet(access_unit, client.video_channel, 96,
                                  last_nal && last_fragment,
                                  client.video_sequence++, timestamp,
                                  client.video_ssrc, prefix, sizeof(prefix),
                                  nal.data + offset, count);
                offset += count;
                first = false;
            }
        }
        if (!access_unit.empty()) enqueue(client, std::move(access_unit), true);
    }

    void drain_media() noexcept {
        std::deque<MediaEvent> events;
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            events.swap(events_);
        }
        for (auto& event : events) {
            if (event.kind == MediaEvent::Kind::audio) {
                const auto timestamp = rtp_timestamp(event.pts_us, 8000);
                for (auto& owned : clients_) {
                    Client& client = *owned;
                    if (!client.failed && !client.backpressured &&
                        client.playing && client.audio_setup)
                        enqueue(client,
                                rtp_packet(client.audio_channel, 8, true,
                                           client.audio_sequence++, timestamp,
                                           client.audio_ssrc, event.audio.data(),
                                           event.audio.size()),
                                true);
                }
                continue;
            }
            try {
                const std::size_t size = RK_MPI_MB_GetSize(event.frame->get());
                if (!size) continue;
                CpuEncodedRead access(event.frame->get());
                const auto* data = static_cast<const std::uint8_t*>(
                    RK_MPI_MB_GetPtr(event.frame->get()));
                if (!data) continue;
                const auto nals = annexb_nals(data, size);
                bool idr = false;
                for (const auto& nal : nals)
                    if (nal.size && (nal.data[0] & 0x1fU) == 5) idr = true;
                const auto timestamp = rtp_timestamp(event.pts_us, 90000);
                for (auto& owned : clients_) {
                    Client& client = *owned;
                    if (client.failed || client.backpressured ||
                        !client.playing || !client.video_setup ||
                        client.stream != event.stream)
                        continue;
                    if (client.waiting_for_idr && !idr) continue;
                    // RequestIDR can overtake already queued VENC output: an
                    // IDR with a newer PTS is sometimes followed by stale P
                    // frames. Sending those frames makes FFmpeg report a
                    // non-monotonic DTS and can corrupt muxer timing. Keep the
                    // RTP timeline strictly forward-moving for each client.
                    if (client.video_timestamp_valid &&
                        !rtp_timestamp_after(timestamp,
                                             client.last_video_timestamp)) {
                        ++client.nonmonotonic_video_drops;
                        if (client.nonmonotonic_video_drops == 1 ||
                            (client.nonmonotonic_video_drops &
                             (client.nonmonotonic_video_drops - 1)) == 0) {
                            std::fprintf(
                                stderr,
                                "rtsp: stream=%d dropped non-monotonic video "
                                "timestamp count=%llu current=%u previous=%u\n",
                                client.stream,
                                static_cast<unsigned long long>(
                                    client.nonmonotonic_video_drops),
                                timestamp, client.last_video_timestamp);
                        }
                        continue;
                    }
                    client.last_video_timestamp = timestamp;
                    client.video_timestamp_valid = true;
                    if (client.waiting_for_idr) client.waiting_for_idr = false;
                    packetize_video(client, nals, timestamp);
                }
            } catch (const std::exception& error) {
                std::fprintf(stderr, "rtsp: video packetization failed: %s\n",
                             error.what());
            }
        }
    }

    int port_ = 0;
    bool audio_ = false;
    std::size_t max_client_output_ = 0;
    int listen_fd_ = -1;
    int wake_[2] = {-1, -1};
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> players_{0};
    std::thread worker_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::mutex events_mutex_;
    std::deque<MediaEvent> events_;
    std::size_t events_high_water_ = 0;
    std::uint64_t input_drops_ = 0;
};

RtspServer::RtspServer(int port, bool audio, std::size_t max_client_output)
    : impl_(new Impl(port, audio, max_client_output)) {}
RtspServer::~RtspServer() = default;
void RtspServer::send(int stream, const Frame& packet) {
    impl_->push_video(stream, packet);
}
void RtspServer::send_audio(const std::vector<std::uint8_t>& packet,
                            std::uint64_t pts_us) {
    impl_->push_audio(packet, pts_us);
}
void RtspServer::poll() {}

}  // namespace vision
