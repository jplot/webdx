// The front end from several pthreads at once, its commands recorded instead of executed.
// Runs under Node: node device_threads.js
#include "device.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

void FillDefaultCaps(D3DCAPS9 *) {}

namespace {

using namespace webdx;

class Recorder final : public Executor {
public:
    void Submit(std::vector<uint8_t> &&commands, std::vector<uint8_t> &&) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chunks_.push_back(std::move(commands));
        submitted_.fetch_add(1);
    }

    uint32_t Completed() const override { return submitted_.load(); }

    // False when the stream holds a command it cannot read.
    bool Replay(std::map<uint32_t, std::vector<uint8_t>> &buffers)
    {
        for (const auto &chunk : chunks_) {
            StreamReader reader(chunk.data(), chunk.size());
            Cmd cmd;
            const uint8_t *p;
            uint32_t size;
            while (reader.Next(cmd, p, size)) {
                if ((uint32_t)cmd > (uint32_t)Cmd::Present) return false;
                if (cmd == Cmd::CreateBuffer || cmd == Cmd::RenameBuffer) {
                    BufferCmd c = StreamReader::Read<BufferCmd>(p);
                    buffers[c.id].assign(c.size, 0);
                } else if (cmd == Cmd::WriteBuffer) {
                    BufferWriteCmd c = StreamReader::Read<BufferWriteCmd>(p);
                    std::vector<uint8_t> &b = buffers[c.id];
                    uint32_t n = size - sizeof c;
                    if (c.offset + n > b.size()) return false;
                    std::memcpy(b.data() + c.offset, p + sizeof c, n);
                }
            }
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<uint8_t>> chunks_;
    std::atomic<uint32_t> submitted_{0};
};

std::atomic<int> g_step{0};

void WaitFor(int step)
{
    while (g_step.load() != step) std::this_thread::yield();
}

uint8_t Pattern(int thread, int round) { return (uint8_t)(1 + thread * 101 + round * 7); }

constexpr int kRounds = 200;
constexpr UINT kHalf = 256;

VertexBuffer *NewBuffer(Device &device, UINT length, DWORD usage)
{
    IDirect3DVertexBuffer9 *vb = nullptr;
    device.CreateVertexBuffer(length, usage, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    return static_cast<VertexBuffer *>(vb);
}

void OverlappingLocks(VertexBuffer *vb)
{
    auto worker = [vb](int self) {
        for (int round = 0; round < kRounds; ++round) {
            int base = round * 4;
            bool releasesFirst = (round % 2 == 0) == (self == 0);
            void *data = nullptr;
            WaitFor(base + self);
            vb->Lock((round * 2 + self) * kHalf, kHalf, &data, D3DLOCK_NOOVERWRITE);
            g_step++;
            WaitFor(base + (releasesFirst ? 2 : 3));
            std::memset(data, Pattern(self, round), kHalf);
            vb->Unlock();
            g_step++;
        }
    };
    std::thread a(worker, 0), b(worker, 1);
    a.join();
    b.join();
}

int CheckOverlappingLocks(const std::vector<uint8_t> &contents)
{
    int failures = 0;
    for (int round = 0; round < kRounds; ++round)
        for (int self = 0; self < 2; ++self) {
            const uint8_t *region = contents.data() + (round * 2 + self) * kHalf;
            for (UINT i = 0; i < kHalf; ++i)
                if (region[i] != Pattern(self, round)) {
                    if (failures++ < 5)
                        printf("[device_threads] round %d thread %d: byte %u is %u, wrote %u\n", round, self, i,
                               region[i], Pattern(self, round));
                    break;
                }
        }
    return failures;
}

constexpr int kWriters = 4, kWrites = 3000, kLoads = 24;
constexpr UINT kWriterSize = 64 << 10, kLoaderSize = 4 << 20;

void RecordWhileFlushing(VertexBuffer **writers, std::vector<uint8_t> *expected, VertexBuffer *loader)
{
    std::vector<std::thread> threads;
    for (int t = 0; t < kWriters; ++t)
        threads.emplace_back([t, vb = writers[t], &shadow = expected[t]] {
            uint32_t seed = 12345u + t;
            shadow.assign(kWriterSize, 0);
            for (int k = 0; k < kWrites; ++k) {
                seed = seed * 1664525u + 1013904223u;
                UINT offset = (seed >> 8) % (kWriterSize / 4) * 4;
                UINT length = std::min<UINT>(4 * (1 + (seed >> 20) % 256), kWriterSize - offset);
                uint8_t value = (uint8_t)(t * 37 + k);
                void *data = nullptr;
                vb->Lock(offset, length, &data, D3DLOCK_NOOVERWRITE);
                std::memset(data, value, length);
                vb->Unlock();
                std::memset(shadow.data() + offset, value, length);
            }
        });
    threads.emplace_back([loader] {
        for (int i = 0; i < kLoads; ++i) {
            void *data = nullptr;
            loader->Lock(0, 0, &data, D3DLOCK_DISCARD);
            std::memset(data, i + 1, kLoaderSize);
            loader->Unlock();
        }
    });
    for (std::thread &t : threads) t.join();
}

} // namespace

int main()
{
    D3DPRESENT_PARAMETERS params = {};
    params.BackBufferWidth = 64;
    params.BackBufferHeight = 64;
    Recorder *recorder = new Recorder;
    Device *device = new Device(recorder, params);

    VertexBuffer *shared = NewBuffer(*device, kRounds * 2 * kHalf, D3DUSAGE_DYNAMIC);
    OverlappingLocks(shared);

    VertexBuffer *writers[kWriters];
    std::vector<uint8_t> expected[kWriters];
    for (VertexBuffer *&w : writers) w = NewBuffer(*device, kWriterSize, D3DUSAGE_DYNAMIC);
    VertexBuffer *loader = NewBuffer(*device, kLoaderSize, D3DUSAGE_DYNAMIC);
    RecordWhileFlushing(writers, expected, loader);

    device->Present(nullptr, nullptr, nullptr, nullptr);
    std::map<uint32_t, std::vector<uint8_t>> buffers;
    if (!recorder->Replay(buffers)) {
        printf("[device_threads] FAILED: unreadable command stream\n");
        return 1;
    }
    int failures = CheckOverlappingLocks(buffers[shared->GpuId()]);
    for (int t = 0; t < kWriters; ++t)
        if (buffers[writers[t]->GpuId()] != expected[t]) {
            printf("[device_threads] writer %d: contents differ from what it wrote\n", t);
            ++failures;
        }
    const std::vector<uint8_t> &loaded = buffers[loader->GpuId()];
    if (loaded.size() != kLoaderSize || loaded.front() != kLoads || loaded.back() != kLoads) {
        printf("[device_threads] loader: last upload missing\n");
        ++failures;
    }
    printf("[device_threads] %s: %u submissions, %d failures\n", failures ? "FAILED" : "ok", recorder->Completed(),
           failures);
    return failures ? 1 : 0;
}
