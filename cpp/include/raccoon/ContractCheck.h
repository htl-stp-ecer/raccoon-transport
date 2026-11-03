#pragma once
// Startup check of the transport contract. Semantics: contract/CHECK_SPEC.md.
//
// A component calls startupCheck() once at start. The result is written to a
// status file (not over the transport) and, when refuseStart() is true, the
// component must exit with kExitCode.

#include <optional>
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
        ComponentRefused,  // the component's own check refused (its name is in CheckResult::result)
        Skipped,
    };

    // The result of a check that belongs to the component, not to the transport (for
    // example "is my version the one in the bundle?"). The component runs it and hands
    // it to check()/startupCheck(). `result` is "ok", "dev-warnung" or any name that
    // refuses (e.g. "version-mismatch"); `message` is shown as it is. It is merged after
    // the transport's own checks (a transport failure always wins) and recorded in the
    // status file. The transport knows nothing about what it checks.
    struct ComponentCheck
    {
        std::string name;    // e.g. "version"
        std::string result;
        std::string message;
        std::string reason;  // may be empty
        std::string expected;  // may be empty
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
        // The component's own check, if it has one (see ComponentCheck).
        std::optional<ComponentCheck> componentCheck;
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
        std::string reason;                    // file-missing, file-unreadable, field-missing, field-empty, or
                                               // the reason of the component's check, or empty
        std::string result;                    // the name used in status files: ok, mismatch, ...
        std::string message;
        std::string bundleFile;
        std::optional<ComponentCheck> componentCheck;

        [[nodiscard]] bool refuseStart() const noexcept
        {
            return outcome == Outcome::Mismatch || outcome == Outcome::BundleError ||
                   outcome == Outcome::ComponentRefused;
        }
    };

    // Name of the transport's own outcomes: ok, mismatch, dev-warnung, bundle-fehler, skipped.
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
