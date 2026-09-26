// Runs a fuzz target without libFuzzer (GCC/MSVC builds and the CI smoke run).
//
//   fuzz_x <file-or-dir>...        replay a corpus (every file is one input)
//   fuzz_x --random <n> <seed>     n inputs from a fixed seed, sizes 0..4096
//
// With clang, configure -DLPQ_FUZZ_ENGINE=libfuzzer for real coverage-guided
// fuzzing; this driver is then not linked.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

void run_file(const std::filesystem::path& p) {
    std::ifstream           in(p, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--random") {
        const unsigned long        n = std::strtoul(argv[2], nullptr, 10);
        std::mt19937_64            rng(std::strtoull(argv[3], nullptr, 10));
        std::vector<std::uint8_t>  buf;
        for (unsigned long i = 0; i < n; ++i) {
            buf.resize(static_cast<std::size_t>(rng() % 4097U));
            for (auto& b : buf) {
                b = static_cast<std::uint8_t>(rng());
            }
            LLVMFuzzerTestOneInput(buf.data(), buf.size());
        }
        std::printf("%lu random inputs: no crash\n", n);
        return 0;
    }
    std::size_t count = 0;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path p(argv[i]);
        if (std::filesystem::is_directory(p)) {
            for (const auto& e : std::filesystem::directory_iterator(p)) {
                if (e.is_regular_file()) {
                    run_file(e.path());
                    ++count;
                }
            }
        } else {
            run_file(p);
            ++count;
        }
    }
    std::printf("%zu corpus inputs: no crash\n", count);
    return 0;
}
