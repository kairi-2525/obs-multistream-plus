#pragma once

#include <functional>
#include <string>
#include <vector>

#include "obs.hpp"

// One target as shown in OBS's own Stats window.
struct StatsTargetInfo {
    std::string id;
    std::string name;
    // Null until the target has been started at least once.
    OBSOutput output;
};

// Called on the UI thread; returns the targets in dock order.
using StatsTargetProvider = std::function<std::vector<StatsTargetInfo>()>;

// Appends one row per target below the Stream/Recording rows of OBS's native
// Stats window (View > Stats) and Stats dock. OBS has no API for this, so the
// rows are added to its widgets from the outside; when the expected layout
// isn't found nothing is changed. The statistics above the output table are
// left alone.
void InstallStatsIntegration(StatsTargetProvider provider);

// Removes the rows again and stops watching for Stats windows. Must run before
// the module is unloaded, as Qt would otherwise keep calling into it.
void UninstallStatsIntegration();
