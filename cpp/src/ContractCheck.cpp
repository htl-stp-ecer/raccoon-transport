// Startup contract check. See contract/CHECK_SPEC.md; the shared test vectors
// in contract/check_vectors.json are run by cpp/tests/test_contract_check.cpp.
//
// JSON parsing uses nlohmann/json (MIT, vendored in cpp/third_party/nlohmann,
// see THIRD_PARTY_NOTICES.md). It is only included here, never in a header.

#include "raccoon/ContractCheck.h"

#include "raccoon/Contract.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace raccoon::contract
{
    namespace
    {
        using json = nlohmann::json;

        constexpr const char* kDefaultBundleFile = "/etc/raccoon-bundle.json";
        constexpr const char* kDefaultDeviceMarker = "/etc/raccoon-image";
        constexpr const char* kDefaultStatusDir = "/run/raccoon";

        std::string pick(const std::string& given, const char* env, const char* fallback)
        {
            if (!given.empty()) return given;
            if (const char* value = std::getenv(env); value != nullptr && *value != '\0') return value;
            return fallback;
        }

        std::string orUnknown(const std::string& value)
        {
            return value.empty() ? "unknown" : value;
        }

        std::string trim(const std::string& text)
        {
            const char* ws = " \t\r\n\f\v";
            const auto first = text.find_first_not_of(ws);
            if (first == std::string::npos) return {};
            return text.substr(first, text.find_last_not_of(ws) - first + 1);
        }

        bool isDirectory(const std::string& path)
        {
            struct stat st{};
            return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }

        const char* reasonText(const std::string& reason)
        {
            if (reason == "file-missing") return "bundle file is missing";
            if (reason == "field-missing") return "bundle file has no contract_hash field";
            if (reason == "field-empty") return "bundle file has an empty contract_hash";
            return "bundle file is unreadable";
        }

        std::string makeMessage(const CheckResult& r)
        {
            switch (r.outcome)
            {
            case Outcome::Ok:
                return "contract ok: " + r.ownHash;
            case Outcome::Skipped:
                return "not running on a raccoon device; contract check skipped";
            case Outcome::DevWarning:
                return "transport contract check is in dev mode (bundle field is 'dev'): " + r.component +
                       " uses contract " + r.ownHash + " (transport " + orUnknown(r.ownTransportVersion) +
                       "); mismatches are not enforced";
            case Outcome::Mismatch:
                return "transport contract mismatch: " + r.component + " " + orUnknown(r.componentVersion) +
                       " uses contract " + r.ownHash + " (transport " + orUnknown(r.ownTransportVersion) +
                       "), bundle " + orUnknown(r.bundle) + " expects " + r.expectedHash + " (transport " +
                       orUnknown(r.expectedTransportVersion) + "); update or reflash so all components match";
            case Outcome::BundleError:
                break;
            }
            return std::string("transport contract check failed: ") + reasonText(r.reason) + ": " + r.bundleFile;
        }

        CheckResult finish(CheckResult r, Outcome outcome, std::string reason = {})
        {
            r.outcome = outcome;
            r.reason = std::move(reason);
            r.message = makeMessage(r);
            return r;
        }

        // Field 22 of /proc/self/stat (clock ticks since boot); -1 if unavailable.
        long long ownStartTime()
        {
            std::ifstream stat("/proc/self/stat");
            std::string line;
            if (!std::getline(stat, line)) return -1;
            const auto close = line.rfind(')');
            if (close == std::string::npos) return -1;
            std::istringstream rest(line.substr(close + 2));
            std::string field;
            for (int i = 3; i <= 22; ++i)
            {
                if (!(rest >> field)) return -1;
            }
            try { return std::stoll(field); } catch (...) { return -1; }
        }

        std::string utcNow()
        {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now, &tm);
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        }

        json nullable(const std::string& value)
        {
            return value.empty() ? json(nullptr) : json(value);
        }
    }

    const char* outcomeName(Outcome outcome) noexcept
    {
        switch (outcome)
        {
        case Outcome::Ok: return "ok";
        case Outcome::Mismatch: return "mismatch";
        case Outcome::DevWarning: return "dev-warnung";
        case Outcome::BundleError: return "bundle-fehler";
        case Outcome::Skipped: return "skipped";
        }
        return "bundle-fehler";
    }

    CheckResult check(const std::string& component, const std::string& componentVersion, const CheckOptions& options)
    {
        CheckResult r;
        r.component = component;
        r.componentVersion = componentVersion;
        r.ownHash = options.ownHash.empty() ? kHash : options.ownHash;
        r.ownTransportVersion = options.ownVersion.empty() ? version() : options.ownVersion;
        r.bundleFile = pick(options.bundleFile, "RACCOON_BUNDLE_FILE", kDefaultBundleFile);
        const std::string marker = pick(options.deviceMarker, "RACCOON_DEVICE_MARKER", kDefaultDeviceMarker);

        const char* forceEnv = std::getenv("RACCOON_CONTRACT_CHECK");
        const bool force = options.force || (forceEnv != nullptr && std::string(forceEnv) == "force");
        if (!force && !isDirectory(marker)) return finish(r, Outcome::Skipped);

        std::ifstream in(r.bundleFile, std::ios::binary);
        if (!in.is_open())
        {
            return finish(r, Outcome::BundleError, errno == ENOENT ? "file-missing" : "file-unreadable");
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        if (in.bad()) return finish(r, Outcome::BundleError, "file-unreadable");

        json document = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
        if (document.is_discarded() || !document.is_object())
        {
            return finish(r, Outcome::BundleError, "file-unreadable");
        }

        if (auto it = document.find("bundle"); it != document.end() && it->is_string()) r.bundle = it->get<std::string>();
        if (auto comps = document.find("components"); comps != document.end() && comps->is_object())
        {
            if (auto tv = comps->find("raccoon-transport"); tv != comps->end() && tv->is_string())
            {
                r.expectedTransportVersion = tv->get<std::string>();
            }
        }

        auto field = document.find("contract_hash");
        if (field == document.end() || !field->is_string()) return finish(r, Outcome::BundleError, "field-missing");
        const std::string expected = trim(field->get<std::string>());
        if (expected.empty()) return finish(r, Outcome::BundleError, "field-empty");
        r.expectedHash = expected;
        if (expected == "dev") return finish(r, Outcome::DevWarning);
        return finish(r, expected == r.ownHash ? Outcome::Ok : Outcome::Mismatch);
    }

    bool writeStatus(const CheckResult& result, bool persistent, const CheckOptions& options)
    {
        const std::string dir = pick(options.statusDir, "RACCOON_STATUS_DIR", kDefaultStatusDir);
        json doc = {
            {"component", result.component},
            {"component_version", result.componentVersion},
            {"own_hash", result.ownHash},
            {"own_transport_version", result.ownTransportVersion},
            {"expected_hash", nullable(result.expectedHash)},
            {"expected_transport_version", nullable(result.expectedTransportVersion)},
            {"bundle", nullable(result.bundle)},
            {"result", outcomeName(result.outcome)},
            {"reason", nullable(result.reason)},
            {"message", result.message},
            {"timestamp", utcNow()},
            {"pid", static_cast<long long>(::getpid())},
            {"pid_starttime", ownStartTime() < 0 ? json(nullptr) : json(ownStartTime())},
            {"persistent", persistent},
        };

        // mkdir -p (one level is enough for /run/raccoon and for tests)
        ::mkdir(dir.c_str(), 0777);
        const std::string tmp = dir + "/." + result.component + "." + std::to_string(::getpid()) + ".tmp";
        const std::string finalPath = dir + "/" + result.component + ".json";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open())
            {
                std::fprintf(stderr, "warning: could not write contract status file %s: %s\n", tmp.c_str(),
                             std::strerror(errno));
                return false;
            }
            out << doc.dump(2) << '\n';
            out.flush();
            if (!out)
            {
                ::unlink(tmp.c_str());
                std::fprintf(stderr, "warning: could not write contract status file %s\n", tmp.c_str());
                return false;
            }
        }
        ::chmod(tmp.c_str(), 0644);
        if (::rename(tmp.c_str(), finalPath.c_str()) != 0)
        {
            std::fprintf(stderr, "warning: could not move contract status file into place: %s\n", std::strerror(errno));
            ::unlink(tmp.c_str());
            return false;
        }
        return true;
    }

    CheckResult startupCheck(const std::string& component, const std::string& componentVersion, bool persistent,
                             const CheckOptions& options)
    {
        CheckResult result = check(component, componentVersion, options);
        if (result.outcome != Outcome::Skipped) writeStatus(result, persistent, options);
        const char* level = result.refuseStart() ? "error" : (result.outcome == Outcome::DevWarning ? "warning" : "info");
        std::fprintf(stderr, "%s: %s: %s\n", level, component.c_str(), result.message.c_str());
        return result;
    }
}
