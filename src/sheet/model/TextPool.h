#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fw::sheet {

// Many short strings in one buffer: 4 bytes each besides their text. Ids count from 0.
class TextPool {
 public:
  uint32_t Add(std::string_view s) {
    data_.append(s);
    ends_.push_back(static_cast<uint32_t>(data_.size()));
    return static_cast<uint32_t>(ends_.size() - 1);
  }
  std::string_view Get(uint32_t id) const {
    if (id >= ends_.size()) return {};
    uint32_t begin = id ? ends_[id - 1] : 0;
    return std::string_view(data_).substr(begin, ends_[id] - begin);
  }
  uint32_t Size() const { return static_cast<uint32_t>(ends_.size()); }
  size_t Bytes() const { return data_.capacity() + ends_.capacity() * sizeof(uint32_t); }
  // Room for more text without passing the 4 GB the offsets can address.
  bool HasRoom(size_t more) const { return data_.size() + more < 0xFFFFFFF0u; }
  void Reserve(size_t strings, size_t bytes) {
    ends_.reserve(strings);
    data_.reserve(bytes);
  }
  void Shrink() {
    data_.shrink_to_fit();
    ends_.shrink_to_fit();
  }

 private:
  std::string data_;
  std::vector<uint32_t> ends_;
};

}  // namespace fw::sheet
