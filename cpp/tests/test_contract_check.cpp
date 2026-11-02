// Runs the shared vectors of contract/check_vectors.json through the C++ check.
#include "raccoon/ContractCheck.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using json = nlohmann::json;
namespace rc = raccoon::contract;

static int failures = 0;

#define EXPECT_EQ(a, b, what)                                                                 \
    do {                                                                                      \
        if (!((a) == (b))) {                                                                  \
            ++failures;                                                                       \
            std::cerr << "FAIL " << what << ": got [" << (a) << "] expected [" << (b) << "]\n"; \
        }                                                                                     \
    } while (0)

static std::string replaceAll(std::string text, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size())
        text.replace(pos, from.size(), to);
    return text;
}

static void runVectors()
{
    std::ifstream in(RACCOON_CHECK_VECTORS);
    std::stringstream buf;
    buf << in.rdbuf();
    const json vectors = json::parse(buf.str());
    const json& d = vectors["defaults"];

    char tmpl[] = "/tmp/raccoon_check_XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    const std::string marker = root + "/image";
    const std::string bundle = root + "/bundle.json";

    for (const auto& c : vectors["cases"])
    {
        const std::string name = c["name"];
        ::rmdir(marker.c_str());
        ::unlink(bundle.c_str());
        if (c["on_device"].get<bool>()) ::mkdir(marker.c_str(), 0755);
        if (!c["bundle"].is_null()) std::ofstream(bundle) << c["bundle"].get<std::string>();

        rc::CheckOptions o;
        o.bundleFile = bundle;
        o.deviceMarker = marker;
        o.ownHash = d["own_hash"];
        o.ownVersion = d["own_version"];
        o.force = c.value("force", false);
        const rc::CheckResult r = rc::check(d["component"], d["component_version"], o);

        EXPECT_EQ(std::string(rc::outcomeName(r.outcome)), c["result"].get<std::string>(), name + " result");
        EXPECT_EQ(r.reason, c["reason"].is_null() ? std::string() : c["reason"].get<std::string>(), name + " reason");
        EXPECT_EQ(r.refuseStart(), c["refuse"].get<bool>(), name + " refuse");
        EXPECT_EQ(r.expectedHash, c["expected_hash"].is_null() ? std::string() : c["expected_hash"].get<std::string>(),
                  name + " expected_hash");
        EXPECT_EQ(replaceAll(r.message, bundle, d["bundle_file"]), c["message"].get<std::string>(), name + " message");
    }
    ::rmdir(marker.c_str());
    ::unlink(bundle.c_str());
    ::rmdir(root.c_str());
}

static void runStatusFile()
{
    char tmpl[] = "/tmp/raccoon_status_XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    rc::CheckOptions o;
    o.statusDir = root + "/run";
    o.bundleFile = root + "/bundle.json";
    o.deviceMarker = root;  // exists, so this counts as a device
    o.ownHash = "aaaa";
    o.ownVersion = "v9";
    std::ofstream(o.bundleFile) << R"({"bundle":"b","contract_hash":"bbbb"})";

    const rc::CheckResult r = rc::startupCheck("stm32-data-reader", "v58", /*persistent=*/true, o);
    EXPECT_EQ(r.refuseStart(), true, "mismatch refuses");

    std::ifstream in(o.statusDir + "/stm32-data-reader.json");
    std::stringstream buf;
    buf << in.rdbuf();
    const json s = json::parse(buf.str(), nullptr, false);
    EXPECT_EQ(s.is_object(), true, "status file is json");
    if (s.is_object())
    {
        EXPECT_EQ(s["result"].get<std::string>(), std::string("mismatch"), "status result");
        EXPECT_EQ(s["persistent"].get<bool>(), true, "status persistent");
        EXPECT_EQ(s["pid"].get<long long>(), static_cast<long long>(::getpid()), "status pid");
        EXPECT_EQ(s["expected_hash"].get<std::string>(), std::string("bbbb"), "status expected_hash");
    }
    EXPECT_EQ(rc::kExitCode, 78, "reserved exit code");

    ::unlink((o.statusDir + "/stm32-data-reader.json").c_str());
    ::rmdir(o.statusDir.c_str());
    ::unlink(o.bundleFile.c_str());
    ::rmdir(root.c_str());
}

int main()
{
    runVectors();
    runStatusFile();
    if (failures == 0) std::cout << "contract check: all ok\n";
    return failures == 0 ? 0 : 1;
}
