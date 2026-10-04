#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "SafeZone.h"

// Writes and reads a SafeZone's device registry (name + safety code
// per member) to/from a .json file — deliberately kept SEPARATE from
// SafeZone itself, so the core SafeZone class doesn't need a JSON
// library or filesystem dependency. This is explicitly a Linux/
// server-side concern, matching the "KR260 as safe zone controller"
// role used throughout this project, not something bare-metal
// firmware would link against.
//
// IMPORTANT, worth being explicit about: readManifest() does NOT, and
// cannot, reconstruct live Safe objects or reattach them to real
// hardware — a Safe& is a runtime reference to something a JSON file
// has no way to encode. What it returns is the (name, safetyCode)
// MANIFEST that was previously saved. The intended use is for the
// zone controller, at startup, to build its real SafeInputs/
// SafeDevices/SafeZones exactly as it always would, then compare what
// it just built against the loaded manifest — catching a missing,
// renamed, or miscoded device before the zone ever starts evaluating,
// rather than silently running with whatever happened to get wired
// up. See verifyMembersMatchManifest() below for that check.
class SafeZoneJsonPersistence {
public:
    struct ManifestEntry {
        std::string name;
        uint32_t    safetyCode;
    };

    // Writes zone's current member registry to filePath as JSON.
    // Returns false on any I/O or serialization failure. Writes to a
    // temp path and renames into place, so a failure partway through
    // (disk full mid-write, say) never leaves filePath holding a
    // truncated or corrupt file.
    static bool writeManifest(const SafeZone& zone, const std::string& zoneName, const std::string& filePath);

    // Reads a previously-written manifest back. Returns false (and
    // leaves the output parameters untouched) if the file doesn't
    // exist, can't be parsed, or doesn't match the expected schema —
    // a malformed entry rejects the whole file rather than silently
    // skipping it, since a partially-trusted safety manifest is worse
    // than an explicitly-failed load.
    static bool readManifest(const std::string& filePath, std::string& outZoneName,
                              std::vector<ManifestEntry>& outEntries);

    // Loads filePath, then verifies zone's CURRENT live members match
    // the saved manifest exactly — same count, same (name,
    // safetyCode) pairs, compared as sets so member-add order doesn't
    // matter. Returns true only on an exact match; any mismatch
    // (missing device, extra device, renamed device, changed code, or
    // a manifest that couldn't be read at all) is reported via
    // outMismatchReason for the caller to log, refuse to start on, or
    // otherwise act on — this function only detects and reports a
    // mismatch, it never decides what should happen because of one.
    static bool verifyMembersMatchManifest(const SafeZone& zone, const std::string& filePath,
                                            std::string& outMismatchReason);
};
