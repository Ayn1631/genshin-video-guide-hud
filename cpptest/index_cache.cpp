#include "index_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <fstream>
#endif

namespace bgi::detail {
namespace {

constexpr int trees = 5;
using Clock = std::chrono::steady_clock;

long long milliseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::unique_ptr<cv::flann::Index> build_index(const cv::Mat& descriptors, int floor) {
    const auto start = Clock::now();
    auto index = std::make_unique<cv::flann::Index>(descriptors, cv::flann::KDTreeIndexParams(trees));
    std::clog << "SIFT index built (floor " << floor << ", " << milliseconds(start) << " ms)" << std::endl;
    return index;
}

#ifdef _WIN32
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw std::runtime_error("SHA-256 provider unavailable");
        }
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw std::runtime_error("cannot initialize SHA-256");
        }
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    ~Sha256() {
        BCryptDestroyHash(hash_);
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    void add(const void* data, std::size_t size) {
        auto* bytes = static_cast<const unsigned char*>(data);
        while (size) {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 24));
            if (BCryptHashData(hash_, const_cast<unsigned char*>(bytes), chunk, 0) < 0) {
                throw std::runtime_error("failed to hash index data");
            }
            size -= chunk;
            bytes += chunk;
        }
    }

    IndexKey finish() {
        IndexKey digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            throw std::runtime_error("failed to finalize index hash");
        }
        return digest;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

constexpr std::array<char, 8> signature{'B', 'G', 'I', 'F', 'L', 'A', 'N', '1'};

// FLANN reads its own payload from the start of the file. This footer keeps
// the compatibility key and integrity check in the same atomic cache entry.
struct Footer {
    std::array<char, 8> signature;
    IndexKey key;
    IndexKey checksum;
};
static_assert(sizeof(Footer) == 72);

std::string hex(const IndexKey& key) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(key.size() * 2);
    for (const auto byte : key) {
        value.push_back(digits[byte >> 4]);
        value.push_back(digits[byte & 15]);
    }
    return value;
}

IndexKey read_checksum(std::istream& stream, std::uintmax_t count) {
    Sha256 hash;
    std::vector<char> buffer(1024u * 1024);
    while (count) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uintmax_t>(count, buffer.size()));
        if (!stream.read(buffer.data(), static_cast<std::streamsize>(chunk))) {
            throw std::runtime_error("index payload is truncated");
        }
        hash.add(buffer.data(), chunk);
        count -= chunk;
    }
    return hash.finish();
}

bool check_cache(const std::filesystem::path& path, const IndexKey& key) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const auto size = stream.tellg();
    if (size <= static_cast<std::streamoff>(sizeof(Footer))) return false;
    const auto bytes = size - static_cast<std::streamoff>(sizeof(Footer));
    stream.seekg(bytes);
    Footer footer{};
    if (!stream.read(reinterpret_cast<char*>(&footer), sizeof(footer)) ||
        footer.signature != signature || footer.key != key) return false;
    stream.seekg(0);
    return read_checksum(stream, static_cast<std::uintmax_t>(bytes)) == footer.checksum;
}

class CacheLock {
public:
    explicit CacheLock(const std::string& key) {
        const auto name = L"Local\\BetterGI-cpptest-FLANN-" + std::wstring(key.begin(), key.end());
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) throw std::runtime_error("cannot open FLANN cache lock");
        const auto result = WaitForSingleObject(mutex_, 30000);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            CloseHandle(mutex_);
            throw std::runtime_error("FLANN cache lock unavailable");
        }
    }
    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;
    ~CacheLock() {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }

private:
    HANDLE mutex_ = nullptr;
};

void save_cache(const cv::flann::Index& index, const std::filesystem::path& path, const IndexKey& key) {
    static std::atomic<unsigned> sequence{0};
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++);
    try {
        index.save(temporary.u8string());
        std::ifstream payload(temporary, std::ios::binary | std::ios::ate);
        const auto bytes = payload.tellg();
        if (bytes <= 0) throw std::runtime_error("FLANN index file is missing");
        payload.seekg(0);
        const auto checksum = read_checksum(payload, static_cast<std::uintmax_t>(bytes));
        payload.close();
        const Footer footer{signature, key, checksum};
        std::ofstream file(temporary, std::ios::binary | std::ios::app);
        file.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
        file.close();
        if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("cannot publish FLANN cache file");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
#endif

}

std::optional<IndexKey> identify_index(const cv::Mat& descriptors, const void* keypoints, std::size_t keypoint_bytes) {
#ifdef _WIN32
    try {
        Sha256 hash;
        const auto format = "BetterGI FLANN 1 / " + std::to_string(sizeof(void*)) + " / " +
                            std::to_string(trees) + " / " + cv::getBuildInformation();
        hash.add(format.data(), format.size());
        hash.add(&descriptors.rows, sizeof(descriptors.rows));
        hash.add(&descriptors.cols, sizeof(descriptors.cols));
        hash.add(keypoints, keypoint_bytes);
        if (descriptors.isContinuous()) {
            hash.add(descriptors.data, descriptors.total() * descriptors.elemSize());
        } else {
            for (int i = 0; i < descriptors.rows; ++i) {
                hash.add(descriptors.ptr(i), descriptors.cols * descriptors.elemSize());
            }
        }
        return hash.finish();
    } catch (const std::exception& e) {
        std::clog << "SIFT cache key unavailable: " << e.what() << std::endl;
    }
#else
    (void)descriptors;
    (void)keypoints;
    (void)keypoint_bytes;
#endif
    return std::nullopt;
}

std::unique_ptr<cv::flann::Index> load_or_build_index(const cv::Mat& descriptors,
                                                    const std::filesystem::path& directory,
                                                    const std::optional<IndexKey>& key, int floor) {
#ifdef _WIN32
    if (!directory.empty() && key) {
        try {
            std::filesystem::create_directories(directory);
            const auto id = hex(*key);
            const CacheLock guard(id);
            const auto path = directory / ("Teyvat-floor-" + std::to_string(floor) + "-" + id + ".flann");
            const auto start = Clock::now();
            try {
                if (check_cache(path, *key)) {
                    auto index = std::make_unique<cv::flann::Index>();
                    if (index->load(descriptors, path.u8string()) &&
                        index->getAlgorithm() == cvflann::FLANN_INDEX_KDTREE &&
                        index->getDistance() == cvflann::FLANN_DIST_L2) {
                        std::clog << "SIFT index loaded from cache (floor " << floor << ", "
                                  << milliseconds(start) << " ms)" << std::endl;
                        return index;
                    }
                }
            } catch (const std::exception& e) {
                std::clog << "SIFT cache read failed: " << e.what() << std::endl;
            }
            if (std::filesystem::is_regular_file(path)) {
                std::clog << "SIFT cache invalid (floor " << floor << "), rebuilding" << std::endl;
            }
            auto index = build_index(descriptors, floor);
            try {
                const auto save_start = Clock::now();
                save_cache(*index, path, *key);
                std::clog << "SIFT index saved (floor " << floor << ", " << milliseconds(save_start) << " ms)"
                          << std::endl;
            } catch (const std::exception& e) {
                std::clog << "SIFT cache write failed, keeping the memory index: " << e.what() << std::endl;
            }
            return index;
        } catch (const std::filesystem::filesystem_error& e) {
            std::clog << "SIFT cache unavailable: " << e.what() << std::endl;
        } catch (const std::runtime_error& e) {
            std::clog << "SIFT cache unavailable: " << e.what() << std::endl;
        }
    }
#else
    (void)directory;
    (void)key;
#endif
    return build_index(descriptors, floor);
}

}
