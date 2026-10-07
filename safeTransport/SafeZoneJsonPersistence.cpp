#include "SafeZoneJsonPersistence.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>
#include <algorithm>

using json = nlohmann::json;

bool SafeZoneJsonPersistence::writeManifest(const SafeZone& zone, const std::string& zoneName,
                                             const std::string& filePath) {
    json j;
    j["zone_name"] = zoneName;
    j["devices"] = json::array();
    for (size_t i = 0; i < zone.memberCount(); ++i) {
        const SafeZoneMember& m = zone.memberAt(i);
        j["devices"].push_back({{"name", m.name}, {"safety_code", m.safetyCode}});
    }

    const std::string tempPath = filePath + ".tmp";
    {
        std::ofstream out(tempPath);
        if (!out) return false;
        out << j.dump(2);
        if (!out) return false; // catches a write failure, not just an open failure
    }
    if (std::rename(tempPath.c_str(), filePath.c_str()) != 0) {
        std::remove(tempPath.c_str()); // don't leave a stray .tmp file behind on failure
        return false;
    }
    return true;
}

bool SafeZoneJsonPersistence::readManifest(const std::string& filePath, std::string& outZoneName,
                                            std::vector<ManifestEntry>& outEntries) {
    std::ifstream in(filePath);
    if (!in) return false;

    json j;
    try {
        in >> j;
    } catch (const json::parse_error&) {
        return false;
    }

    if (!j.contains("zone_name") || !j.contains("devices") || !j["devices"].is_array()) {
        return false;
    }

    std::vector<ManifestEntry> entries;
    for (const auto& device : j["devices"]) {
        if (!device.contains("name") || !device.contains("safety_code")) {
            return false; // malformed entry -- reject the whole file, don't silently skip it
        }
        try {
            entries.push_back(ManifestEntry{device.at("name").get<std::string>(),
                                             device.at("safety_code").get<uint32_t>()});
        } catch (const json::exception&) {
            return false; // wrong type for a field -- same "reject the whole file" treatment
        }
    }

    outZoneName = j["zone_name"].get<std::string>();
    outEntries = std::move(entries);
    return true;
}

bool SafeZoneJsonPersistence::verifyMembersMatchManifest(const SafeZone& zone, const std::string& filePath,
                                                           std::string& outMismatchReason) {
    std::string savedZoneName;
    std::vector<ManifestEntry> saved;
    if (!readManifest(filePath, savedZoneName, saved)) {
        outMismatchReason = "could not read or parse manifest at " + filePath;
        return false;
    }

    if (saved.size() != zone.memberCount()) {
        outMismatchReason = "member count mismatch: manifest has " + std::to_string(saved.size()) +
                             ", zone currently has " + std::to_string(zone.memberCount());
        return false;
    }

    std::vector<ManifestEntry> current;
    current.reserve(zone.memberCount());
    for (size_t i = 0; i < zone.memberCount(); ++i) {
        const SafeZoneMember& m = zone.memberAt(i);
        current.push_back(ManifestEntry{m.name, m.safetyCode});
    }

    // Compared as sets, not sequences: a zone rebuilt from scratch at
    // startup might add its members in a different order than
    // whatever order they happened to be in when last saved.
    auto byName = [](const ManifestEntry& a, const ManifestEntry& b) { return a.name < b.name; };
    std::sort(saved.begin(), saved.end(), byName);
    std::sort(current.begin(), current.end(), byName);

    for (size_t i = 0; i < saved.size(); ++i) {
        if (saved[i].name != current[i].name) {
            outMismatchReason = "device mismatch: manifest expects '" + saved[i].name +
                                 "', zone has '" + current[i].name + "' at the same position";
            return false;
        }
        if (saved[i].safetyCode != current[i].safetyCode) {
            outMismatchReason = "safety code mismatch for '" + saved[i].name + "': manifest says " +
                                 std::to_string(saved[i].safetyCode) + ", zone currently has " +
                                 std::to_string(current[i].safetyCode);
            return false;
        }
    }

    outMismatchReason.clear();
    return true;
}
