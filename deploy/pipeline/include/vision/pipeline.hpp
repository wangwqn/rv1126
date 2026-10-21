#pragma once

#include "vision/config.hpp"
#include <memory>
#include <string>

namespace vision {
class Pipeline {
public:
    explicit Pipeline(Config config);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    void start();
    void stop() noexcept;
    bool failed() const;
    std::string error() const;
    std::string statistics() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vision
