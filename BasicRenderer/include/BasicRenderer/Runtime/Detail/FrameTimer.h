#include <chrono>

class FrameTimer {
public:
    FrameTimer()
        : _last(std::chrono::steady_clock::now())
    {
    }

    float tick() {
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> delta = now - _last;
        _last = now;
        return delta.count();
    }

private:
    std::chrono::time_point<std::chrono::steady_clock> _last;
};
