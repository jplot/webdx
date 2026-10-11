// stream.h — the command stream the Direct3D 9 front end records and an executor replays.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace webdx {

enum class Cmd : uint32_t {
    Clear,      // ClearCmd: clear the back buffer
    Present,    // no payload
};

struct ClearCmd {
    float color[4];
};

class StreamWriter {
public:
    template <class T> void Put(Cmd cmd, const T &payload)
    {
        Header(cmd, sizeof(T));
        Append(&payload, sizeof(T));
    }
    void Put(Cmd cmd) { Header(cmd, 0); }
    std::vector<uint8_t> Take()
    {
        std::vector<uint8_t> out;
        out.swap(bytes_);
        return out;
    }
    size_t Size() const { return bytes_.size(); }

private:
    void Header(Cmd cmd, uint32_t size)
    {
        uint32_t h[2] = {(uint32_t)cmd, size};
        Append(h, sizeof h);
    }
    void Append(const void *p, size_t n)
    {
        size_t at = bytes_.size();
        bytes_.resize(at + n);
        std::memcpy(bytes_.data() + at, p, n);
    }
    std::vector<uint8_t> bytes_;
};

class StreamReader {
public:
    StreamReader(const uint8_t *p, size_t n) : p_(p), end_(p + n) {}
    bool Next(Cmd &cmd, const uint8_t *&payload, uint32_t &size)
    {
        if (p_ + 8 > end_) return false;
        uint32_t h[2];
        std::memcpy(h, p_, sizeof h);
        cmd = (Cmd)h[0];
        size = h[1];
        payload = p_ + 8;
        p_ += 8 + size;
        return true;
    }
    template <class T> static T Read(const uint8_t *payload)
    {
        T t;
        std::memcpy(&t, payload, sizeof t);
        return t;
    }

private:
    const uint8_t *p_;
    const uint8_t *end_;
};

} // namespace webdx
