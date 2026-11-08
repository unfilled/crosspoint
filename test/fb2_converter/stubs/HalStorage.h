#pragma once

// Host stub for StoredZipWriter tests: FILE-backed HalFile/HalStorage with
// just the surface the writer uses.

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const char* path, const char* mode) {
    close();
    file_ = std::fopen(path, mode);
    return file_ != nullptr;
  }
  size_t write(const void* buffer, size_t count) { return file_ ? std::fwrite(buffer, 1, count, file_) : 0; }
  bool seekSet(size_t pos) { return file_ && std::fseek(file_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool flush() { return file_ && std::fflush(file_) == 0; }
  bool close() {
    if (!file_) return false;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "wb"); }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
};

#define Storage HalStorage::getInstance()
