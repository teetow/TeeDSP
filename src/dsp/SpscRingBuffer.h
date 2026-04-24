#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace dsp {

// Lock-free single-producer/single-consumer ring buffer of floats.
// Capacity is rounded up to the next power of two so we can mask instead of mod.
class SpscRingBuffer
{
public:
    explicit SpscRingBuffer(std::size_t capacity = 0)
    {
        resize(capacity);
    }

    void resize(std::size_t capacity)
    {
        std::size_t pow2 = 1;
        while (pow2 < capacity)
            pow2 <<= 1;
        if (pow2 < 1024) pow2 = 1024;
        m_buf.assign(pow2, 0.0f);
        m_mask = pow2 - 1;
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    void clear()
    {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
        std::memset(m_buf.data(), 0, m_buf.size() * sizeof(float));
    }

    std::size_t capacity() const { return m_buf.size(); }

    std::size_t available() const
    {
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        return head - tail;
    }

    std::size_t space() const { return capacity() - available(); }

    // Producer side. Returns the number of samples actually written.
    std::size_t write(const float *src, std::size_t count)
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        const std::size_t free = capacity() - (head - tail);
        const std::size_t toWrite = count < free ? count : free;
        for (std::size_t i = 0; i < toWrite; ++i)
            m_buf[(head + i) & m_mask] = src[i];
        m_head.store(head + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Consumer side. Returns the number of samples actually read.
    std::size_t read(float *dst, std::size_t count)
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t avail = head - tail;
        const std::size_t toRead = count < avail ? count : avail;
        for (std::size_t i = 0; i < toRead; ++i)
            dst[i] = m_buf[(tail + i) & m_mask];
        m_tail.store(tail + toRead, std::memory_order_release);
        return toRead;
    }

private:
    std::vector<float> m_buf;
    std::size_t m_mask = 0;
    std::atomic<std::size_t> m_head{0};
    std::atomic<std::size_t> m_tail{0};
};

} // namespace dsp
