#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class LuaRunner
{
public:
    using OutputCallback =
        std::function<void(const char*, std::size_t)>;

    using FinishedCallback =
        std::function<void(bool)>;

    LuaRunner() = default;
    ~LuaRunner();

    LuaRunner(const LuaRunner&) = delete;
    LuaRunner& operator=(const LuaRunner&) = delete;

    bool Run(
        std::string source,
        std::string chunkName,
        std::string moduleDirectory,
        OutputCallback outputCallback,
        FinishedCallback finishedCallback
    );

    void Stop() noexcept;
    void Join();

    [[nodiscard]] bool IsRunning() const noexcept;

private:
    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
};