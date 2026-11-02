#pragma once
// Startup check of the transport contract. Semantics: contract/CHECK_SPEC.md.
//
// A component calls startupCheck() once at start. The result is written to a
// status file (not over the transport) and, when refuseStart() is true, the
// component must exit with kExitCode.

#include <string>

namespace raccoon::contract
{
    // Reserved exit code of a component that refuses to start; systemd units
    // set RestartPreventExitStatus=78 so a refusal is not restarted in a loop.
    inline constexpr int kExitCode = 78;

    enum class Outcome
    {
        Ok,
        Mismatch,
        DevWarning,
        BundleError,
        Skipped,
    };

    struct CheckOptions
    {
        // Empty = environment variable, then the default path of the spec.
        std::string bundleFile;      // RACCOON_BUNDLE_FILE,   /etc/raccoon-bundle.json
        std::string deviceMarker;    // RACCOON_DEVICE_MARKER, /etc/raccoon-image
        std::string statusDir;       // RACCOON_STATUS_DIR,    /run/raccoon
        // Empty = the values compiled into this library (tests override them).
        std::string ownHash;
        std::string ownVersion;
        bool force = false;          // also RACCOON_CONTRACT_CHECK=force
    };

    struct CheckResult
    {
        std::string component;
        std::string componentVersion;
        std::string ownHash;
        std::string ownTransportVersion;
        std::string expectedHash;              // empty if unknown
        std::string expectedTransportVersion;  // empty if unknown
        std::string bundle;                    // empty if unknown
        Outcome outcome = Outcome::Skipped;
        std::string reason;                    // file-missing, file-unreadable, field-missing, field-empty, or empty
        std::string message;
        std::string bundleFile;

        [[nodiscard]] bool refuseStart() const noexcept
        {
            return outcome == Outcome::Mismatch || outcome == Outcome::BundleError;
        }
    };

    // Name used in status files and logs: ok, mismatch, dev-warnung, bundle-fehler, skipped.
    const char* outcomeName(Outcome outcome) noexcept;

    // The decision of the spec. Reads one file, writes nothing.
    CheckResult check(const std::string& component, const std::string& componentVersion,
                      const CheckOptions& options = {});

    // Writes <statusDir>/<component>.json atomically. Returns false (and
    // prints a warning to stderr) on failure; never throws.
    bool writeStatus(const CheckResult& result, bool persistent, const CheckOptions& options = {});

    // check() + writeStatus() + one line to stderr (error for a refusal,
    // warning for dev mode). The caller decides what to do with refuseStart().
    CheckResult startupCheck(const std::string& component, const std::string& componentVersion,
                             bool persistent, const CheckOptions& options = {});
}
