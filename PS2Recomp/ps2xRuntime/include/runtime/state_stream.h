#pragma once
// Save states: a flat little-endian byte stream with section tags. Every subsystem writes its
// part with StateWriter and reads it back with StateReader; a reader that runs past its data or
// meets an unexpected tag fails (ok() false) instead of reading garbage.
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace ps2state
{
    class StateWriter
    {
    public:
        std::vector<uint8_t> bytes;

        void raw(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            bytes.insert(bytes.end(), p, p + size);
        }
        template <typename T>
        void pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            raw(&value, sizeof(T));
        }
        void u8(uint8_t v) { pod(v); }
        void u32(uint32_t v) { pod(v); }
        void u64(uint64_t v) { pod(v); }
        void i32(int32_t v) { pod(v); }
        void flag(bool v) { u8(v ? 1u : 0u); }
        void tag(const char (&name)[5]) { raw(name, 4); }
        void blob(const std::vector<uint8_t> &v)
        {
            u64(v.size());
            raw(v.data(), v.size());
        }
    };

    class StateReader
    {
    public:
        StateReader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

        bool ok() const { return m_ok; }
        const std::string &error() const { return m_error; }
        size_t offset() const { return m_at; }
        void fail(const std::string &why)
        {
            if (m_ok)
                m_error = why + " at byte " + std::to_string(m_at);
            m_ok = false;
        }
        bool raw(void *out, size_t size)
        {
            if (!m_ok || size > m_size - m_at)
            {
                fail("truncated state");
                std::memset(out, 0, size);
                return false;
            }
            std::memcpy(out, m_data + m_at, size);
            m_at += size;
            return true;
        }
        template <typename T>
        T pod()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            T value{};
            raw(&value, sizeof(T));
            return value;
        }
        uint8_t u8() { return pod<uint8_t>(); }
        uint32_t u32() { return pod<uint32_t>(); }
        uint64_t u64() { return pod<uint64_t>(); }
        int32_t i32() { return pod<int32_t>(); }
        bool flag() { return u8() != 0u; }
        bool tag(const char (&name)[5])
        {
            char got[4]{};
            raw(got, 4);
            if (m_ok && std::memcmp(got, name, 4) != 0)
                fail(std::string("expected section ") + name);
            return m_ok;
        }
        bool blob(std::vector<uint8_t> &out, size_t limit = size_t(64) << 20)
        {
            const uint64_t n = u64();
            if (!m_ok || n > limit || n > m_size - m_at)
            {
                fail("bad blob size");
                return false;
            }
            out.assign(m_data + m_at, m_data + m_at + n);
            m_at += size_t(n);
            return true;
        }

    private:
        const uint8_t *m_data;
        size_t m_size;
        size_t m_at = 0;
        bool m_ok = true;
        std::string m_error;
    };
}
