#pragma once

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace nemo::test {
// Process-local test configuration; restore even when an assertion returns
// early. Environment-mutating tests must not run concurrently in one process.
class ScopedEnvironment {
public:
    ScopedEnvironment(std::string name, const std::optional<std::string>& value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str()))
            previous_ = previous;
        if (update(value) != 0)
            throw std::runtime_error("cannot set test environment variable " + name_);
    }
    ~ScopedEnvironment() { (void)update(previous_); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    int update(const std::optional<std::string>& value) const {
#if defined(_WIN32)
        return _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
        return value ? setenv(name_.c_str(), value->c_str(), 1) : unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    std::optional<std::string> previous_;
};
}  // namespace nemo::test
