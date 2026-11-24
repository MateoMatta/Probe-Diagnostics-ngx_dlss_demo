#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

class FramePacer
{
    using Clock = std::chrono::steady_clock;
    using ErrorCallback = void (*)(const char*, unsigned long);
    Clock::time_point m_LastStart {};
    ErrorCallback m_OnError;
#ifdef _WIN32
    HANDLE m_Timer = nullptr;
#endif

public:
    explicit FramePacer(ErrorCallback onError) : m_OnError(onError)
    {
#ifdef _WIN32
        m_Timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!m_Timer)
            m_OnError("create high-resolution frame timer", GetLastError());
#endif
    }

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    ~FramePacer()
    {
#ifdef _WIN32
        if (m_Timer)
            CloseHandle(m_Timer);
#endif
    }

    int64_t wait(unsigned fps)
    {
        const auto period = std::chrono::ceil<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        if (m_LastStart != Clock::time_point {})
        {
            const auto deadline = m_LastStart + period;
            auto now = Clock::now();
            while (now < deadline)
            {
#ifdef _WIN32
                if (m_Timer)
                {
                    using HundredNanoseconds = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
                    LARGE_INTEGER due {};
                    due.QuadPart = -std::chrono::ceil<HundredNanoseconds>(deadline - now).count();
                    if (!SetWaitableTimer(m_Timer, &due, 0, nullptr, nullptr, FALSE))
                    {
                        m_OnError("set high-resolution frame timer", GetLastError());
                        CloseHandle(m_Timer);
                        m_Timer = nullptr;
                    }
                    else
                    {
                        const DWORD result = WaitForSingleObject(m_Timer, INFINITE);
                        if (result != WAIT_OBJECT_0)
                        {
                            m_OnError("wait for high-resolution frame timer",
                                      result == WAIT_FAILED ? GetLastError() : result);
                            CloseHandle(m_Timer);
                            m_Timer = nullptr;
                        }
                    }
                }
                else
#endif
                    std::this_thread::sleep_until(deadline);
                now = Clock::now();
            }
        }
        const auto current = Clock::now();
        const int64_t interval = m_LastStart == Clock::time_point {} ? 0 :
            std::chrono::duration_cast<std::chrono::nanoseconds>(current - m_LastStart).count();
        m_LastStart = current;
        return interval;
    }
};
