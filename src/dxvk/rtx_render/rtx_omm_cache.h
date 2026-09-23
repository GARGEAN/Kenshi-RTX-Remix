#pragma once

// Portable opacity states only. Vulkan objects and geometry placement never
// enter these files. Keep this header independent of the renderer for tests.
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace dxvk::ommcache {
  using Triangle = std::array<uint32_t, 6>;
  using Key = std::array<uint64_t, 2>;
  constexpr uint64_t kVersion = 1;
  constexpr uint64_t kMaxBytes = 64ull * 1024 * 1024;

  struct Layout {
    std::vector<Triangle> triangles;
    std::vector<uint32_t> indices;
  };

  inline Layout deduplicate(const std::vector<Triangle>& source) {
    Layout result;
    result.triangles = source;
    std::sort(result.triangles.begin(), result.triangles.end());
    result.triangles.erase(std::unique(result.triangles.begin(), result.triangles.end()), result.triangles.end());
    result.indices.reserve(source.size());
    for (const auto& triangle : source)
      result.indices.push_back(uint32_t(std::lower_bound(result.triangles.begin(), result.triangles.end(), triangle) - result.triangles.begin()));
    return result;
  }

  inline bool readLayout(const void* uvData, uint64_t uvLength, uint64_t uvStride,
      const void* indexData, uint64_t indexLength, uint64_t indexStride, uint32_t indexWidth,
      uint32_t vertices, uint32_t count, Layout& result) {
    if (!uvData || uvStride < 8 || !count || count > (1u << 20)) return false;
    if (indexWidth && (!indexData || (indexWidth != 2 && indexWidth != 4) || indexStride < indexWidth ||
        (uint64_t(count) * 3 - 1) * indexStride + indexWidth > indexLength)) return false;
    std::vector<Triangle> triangles(count);
    for (uint32_t triangle = 0; triangle < count; ++triangle) {
      for (uint32_t corner = 0; corner < 3; ++corner) {
        uint32_t vertex = triangle * 3 + corner;
        if (indexWidth) {
          const uint64_t offset = uint64_t(vertex) * indexStride;
          vertex = 0;
          std::memcpy(&vertex, static_cast<const uint8_t*>(indexData) + offset, indexWidth);
        }
        if (vertex >= vertices || uint64_t(vertex) * uvStride + 8 > uvLength) return false;
        std::memcpy(&triangles[triangle][corner * 2], static_cast<const uint8_t*>(uvData) + uint64_t(vertex) * uvStride, 8);
        for (uint32_t component = 0; component < 2; ++component)
          if ((triangles[triangle][corner * 2 + component] & 0x7f800000u) == 0x7f800000u) return false;
      }
    }
    result = deduplicate(triangles);
    return true;
  }

  inline uint64_t checksum(const std::vector<uint8_t>& data) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : data) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
  }

  inline std::string filename(const Key& key) {
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << key[0] << std::setw(16) << key[1] << ".ommbin";
    return s.str();
  }

  inline void writeWord(std::ostream& stream, uint64_t value) {
    char bytes[8];
    for (unsigned i = 0; i != 8; ++i) bytes[i] = char(value >> (8 * i));
    stream.write(bytes, 8);
  }
  inline uint64_t readWord(std::istream& stream) {
    unsigned char bytes[8] = {};
    stream.read(reinterpret_cast<char*>(bytes), 8);
    uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
  }

  inline bool read(const std::filesystem::path& path, const Key& key, uint64_t expectedBytes, std::vector<uint8_t>& data) {
    if (!expectedBytes || expectedBytes > kMaxBytes) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() != std::streamoff(48 + expectedBytes)) return false;
    f.seekg(0);
    const uint64_t magic = readWord(f), version = readWord(f);
    const Key stored { readWord(f), readWord(f) };
    const uint64_t size = readWord(f), hash = readWord(f);
    if (!f || magic != 0x3145484341434d4full || version != kVersion || stored != key || size != expectedBytes) return false;
    data.resize(size_t(size));
    f.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    if (!f || checksum(data) != hash) { std::vector<uint8_t>().swap(data); return false; }
    return true;
  }

  inline bool write(const std::filesystem::path& path, const Key& key, const std::vector<uint8_t>& data) {
    if (data.empty() || data.size() > kMaxBytes) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    {
      std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      writeWord(f, 0x3145484341434d4full); writeWord(f, kVersion);
      writeWord(f, key[0]); writeWord(f, key[1]);
      writeWord(f, data.size()); writeWord(f, checksum(data));
      f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      f.flush();
      if (!f) return false;
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
      // Windows cannot rename over an existing corrupt/obsolete record. Any
      // interruption here is a cache miss; partially written data is never used.
      std::filesystem::remove(path, ec);
      ec.clear();
      std::filesystem::rename(temporary, path, ec);
    }
    return !ec;
  }

  enum class State : uint8_t { Pending, Hit, Miss };
  struct Record {
    std::atomic<State> state { State::Pending };
    std::vector<uint8_t> bytes; // Immutable after Hit is published.
    uint64_t expectedBytes = 0;
    uint64_t reservedBytes = 0; // Store mutex protects accounting, including completed misses.
  };

  class Store {
  public:
    explicit Store(std::filesystem::path folder) : m_folder(std::move(folder)), m_worker([this] { run(); }) { }
    ~Store() {
      { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; }
      m_condition.notify_one();
      m_worker.join();
    }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::shared_ptr<Record> request(const Key& key, uint64_t bytes) {
      if (!bytes || bytes > kMaxBytes) return {};
      std::lock_guard<std::mutex> lock(m_mutex);
      auto found = m_records.find(key);
      if (found != m_records.end()) return found->second;
      trim(bytes);
      if (m_jobs.size() >= 8 || m_reservedBytes + bytes > kMaxBytes || m_records.size() >= 128) return {};
      auto record = std::make_shared<Record>();
      record->expectedBytes = bytes;
      record->reservedBytes = bytes;
      m_records.emplace(key, record);
      m_reservedBytes += bytes;
      // Warm reads must not sit behind queued writes from cold bakes.
      auto firstWrite = std::find_if(m_jobs.begin(), m_jobs.end(), [](const Job& job) { return job.save; });
      m_jobs.insert(firstWrite, { key, record, false });
      m_condition.notify_one();
      return record;
    }

    void publish(const Key& key, std::vector<uint8_t> bytes) {
      if (bytes.empty() || bytes.size() > kMaxBytes) return;
      auto record = std::make_shared<Record>();
      record->expectedBytes = bytes.size();
      record->reservedBytes = bytes.size();
      record->bytes = std::move(bytes);
      record->state.store(State::Hit, std::memory_order_release);
      std::lock_guard<std::mutex> lock(m_mutex);
      auto old = m_records.find(key);
      if (old != m_records.end()) { m_reservedBytes -= old->second->reservedBytes; m_records.erase(old); }
      trim(record->expectedBytes);
      if (m_reservedBytes + record->expectedBytes <= kMaxBytes && m_records.size() < 128) {
        m_records.emplace(key, record);
        m_reservedBytes += record->expectedBytes;
      }
      if (m_jobs.size() < 8 && m_queuedWriteBytes + record->expectedBytes <= kMaxBytes) {
        m_queuedWriteBytes += record->expectedBytes;
        m_jobs.push_back({ key, record, true });
        m_condition.notify_one();
      }
    }

    std::atomic<uint64_t> diskHits { 0 }, diskMisses { 0 }, writes { 0 }, writeFailures { 0 };

  private:
    struct Job { Key key; std::shared_ptr<Record> record; bool save; };
    const std::filesystem::path m_folder;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::map<Key, std::shared_ptr<Record>> m_records;
    std::deque<Job> m_jobs;
    uint64_t m_reservedBytes = 0, m_queuedWriteBytes = 0;
    bool m_stop = false;
    std::thread m_worker;

    void trim(uint64_t incoming) {
      for (auto it = m_records.begin(); it != m_records.end() &&
          (m_reservedBytes + incoming > kMaxBytes || m_records.size() >= 128);) {
        const auto state = it->second->state.load(std::memory_order_acquire);
        if (state == State::Miss || (it->second.use_count() == 1 && state != State::Pending)) {
          m_reservedBytes -= it->second->reservedBytes;
          it = m_records.erase(it);
        } else { ++it; }
      }
    }

    void run() {
      for (;;) {
        Job job;
        {
          std::unique_lock<std::mutex> lock(m_mutex);
          m_condition.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
          if (m_jobs.empty()) return;
          job = std::move(m_jobs.front()); m_jobs.pop_front();
        }
        try {
          const auto path = m_folder / filename(job.key);
          if (job.save) {
            if (write(path, job.key, job.record->bytes)) ++writes; else ++writeFailures;
          } else {
            const bool hit = read(path, job.key, job.record->expectedBytes, job.record->bytes);
            if (hit) ++diskHits; else ++diskMisses;
            job.record->state.store(hit ? State::Hit : State::Miss, std::memory_order_release);
          }
        } catch (...) {
          if (job.save) ++writeFailures;
          else {
            ++diskMisses;
            std::vector<uint8_t>().swap(job.record->bytes);
            job.record->state.store(State::Miss, std::memory_order_release);
          }
        }
        if (!job.save && job.record->state.load(std::memory_order_acquire) == State::Miss) {
          // An empty miss may remain referenced by a many-frame cold bake.
          // It must not reserve the bytes needed to read unrelated warm files.
          std::lock_guard<std::mutex> lock(m_mutex);
          const auto current = m_records.find(job.key);
          if (current != m_records.end() && current->second == job.record) {
            m_reservedBytes -= job.record->reservedBytes;
            job.record->reservedBytes = 0;
          }
        }
        if (job.save) {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_queuedWriteBytes -= job.record->expectedBytes;
        }
      }
    }
  };
}
