#include "interop_common.h"
#include <raccoon/Transport.h>
#include <raccoon/Options.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: interop_subscribe <channel> <type> [options]\n"
                  << "       interop_subscribe --fingerprint <type>\n"
                  << "       interop_subscribe --decode-hex <type> <hex>\n";
        return 1;
    }

    std::string firstArg = argv[1];

    // --fingerprint <type>: print fingerprint and exit
    if (firstArg == "--fingerprint") {
        if (argc < 3) {
            std::cerr << "Usage: interop_subscribe --fingerprint <type>\n";
            return 1;
        }
        std::string type = argv[2];
        int64_t hash = getFingerprint(type);
        uint64_t uhash = static_cast<uint64_t>(hash);
        std::ostringstream oss;
        oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << uhash;
        printEvent("{\"event\":\"fingerprint\",\"value\":\"" + oss.str() + "\"}");
        return 0;
    }

    // --decode-hex <type> <hex>: decode hex bytes and exit
    if (firstArg == "--decode-hex") {
        if (argc < 4) {
            std::cerr << "Usage: interop_subscribe --decode-hex <type> <hex>\n";
            return 1;
        }
        std::string type = argv[2];
        std::string hexStr = argv[3];
        auto bytes = hexToBytes(hexStr);
        std::string json = messageToJson(type, bytes.data(),
                                         static_cast<int>(bytes.size()));
        printEvent("{\"event\":\"decoded\",\"data\":" + json + "}");
        return 0;
    }

    // Normal subscribe mode: <channel> <type> [--count N] [--timeout-ms MS] [--request-retained]
    if (argc < 3) {
        std::cerr << "Usage: interop_subscribe <channel> <type> [options]\n";
        return 1;
    }

    std::string channel = argv[1];
    std::string type = argv[2];

    int count = 1;
    int timeoutMs = 5000;
    bool requestRetained = false;

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc) {
            count = std::stoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeoutMs = std::stoi(argv[++i]);
        } else if (arg == "--request-retained") {
            requestRetained = true;
        }
    }

    auto transport = raccoon::Transport::create();
    int received = 0;

    raccoon::SubscribeOptions opts;
    opts.requestRetained = requestRetained;

    transport.subscribeRaw(channel, [&](const void* data, int dataLen) {
        try {
            std::string json = messageToJson(type, data, dataLen);
            printEvent("{\"event\":\"received\",\"data\":" + json + "}");
            received++;
        } catch (const std::exception& e) {
            std::cerr << "Decode error: " << e.what() << '\n';
        }
    }, opts);

    printEvent("{\"event\":\"subscribed\"}");

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);

    while (received < count &&
           std::chrono::steady_clock::now() < deadline) {
        transport.spinOnce(50);
    }

    if (received >= count) {
        printEvent("{\"event\":\"done\",\"count\":" +
                   std::to_string(received) + "}");
        return 0;
    } else {
        std::cerr << "Timeout after " << timeoutMs
                  << "ms (received " << received << "/" << count << ")\n";
        printEvent("{\"event\":\"timeout\",\"count\":" +
                   std::to_string(received) + "}");
        return 1;
    }
}
