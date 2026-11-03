// A main() for the fuzz targets on toolchains without libFuzzer: runs the target once on each
// file given, or on every file under each folder given. Crashes and aborts show up as they
// would under the fuzzer, so corpora and regression files can be replayed anywhere.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fputs("Usage: <target> <file or folder>...\n", stderr);
    return 2;
  }
  size_t runs = 0;
  auto run = [&](const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    ++runs;
  };
  for (int i = 1; i < argc; ++i) {
    std::filesystem::path path = argv[i];
    if (std::filesystem::is_directory(path)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
        if (entry.is_regular_file()) run(entry.path());
    } else {
      run(path);
    }
  }
  std::printf("Replayed %zu inputs without a crash.\n", runs);
  return 0;
}
