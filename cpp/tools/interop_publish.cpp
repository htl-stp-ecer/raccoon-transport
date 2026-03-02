#include "interop_common.h"
#include <raccoon/Transport.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: interop_publish <channel> <type> <json> [options]\n"
                  << "       interop_publish --encode-hex <type> <json>\n";
        return 1;
    }

    std::string firstArg = argv[1];

    // --encode-hex <type> <json>: encode to hex without network
    if (firstArg == "--encode-hex") {
        if (argc < 4) {
            std::cerr << "Usage: interop_publish --encode-hex <type> <json>\n";
            return 1;
        }
        std::string type = argv[2];
        JsonObject values = parseJson(argv[3]);
        auto encoded = createAndEncode(type, values);
        std::string hex = bytesToHex(encoded.data(), static_cast<int>(encoded.size()));
        printEvent("{\"event\":\"encoded\",\"hex\":\"" + hex + "\"}");
        return 0;
    }

    // Normal publish mode: <channel> <type> <json> [--count N] [--interval-ms MS] [--retained]
    if (argc < 4) {
        std::cerr << "Usage: interop_publish <channel> <type> <json> [options]\n";
        return 1;
    }

    std::string channel = argv[1];
    std::string type = argv[2];
    JsonObject values = parseJson(argv[3]);

    int count = 1;
    int intervalMs = 0;
    bool retained = false;

    for (int i = 4; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc) {
            count = std::stoi(argv[++i]);
        } else if (arg == "--interval-ms" && i + 1 < argc) {
            intervalMs = std::stoi(argv[++i]);
        } else if (arg == "--retained") {
            retained = true;
        }
    }

    auto transport = raccoon::Transport::create();
    raccoon::PublishOptions opts;
    opts.retained = retained;

    for (int seq = 0; seq < count; seq++) {
        auto encoded = createAndEncode(type, values);
        transport.publishRaw(channel, encoded.data(),
                             static_cast<int>(encoded.size()), opts);
        printEvent("{\"event\":\"published\",\"seq\":" +
                   std::to_string(seq) + "}");
        if (intervalMs > 0 && seq < count - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
    }

    if (retained) {
        printEvent("{\"event\":\"ready\"}");
        // Spin transport in background to respond to retain requests
        std::atomic<bool> running{true};
        std::thread spinner([&]() {
            while (running.load(std::memory_order_relaxed)) {
                transport.spinOnce(100);
            }
        });
        // Wait for stdin EOF (parent closes stdin to signal shutdown)
        char c;
        while (std::cin.get(c)) {}
        running.store(false, std::memory_order_relaxed);
        spinner.join();
    }

    return 0;
}
