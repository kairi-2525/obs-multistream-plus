#include "pch.h"
#include "helpers.h"

#include "obs.hpp"

void LoadHotkeyBindings(obs_hotkey_id id, const char* name)
{
    if (id == OBS_INVALID_HOTKEY_ID)
        return;

    OBSDataArrayAutoRelease bindings;
    config_t* profile = obs_frontend_get_profile_config();
    const char* saved = profile ? config_get_string(profile, "Hotkeys", name) : nullptr;
    if (saved && *saved) {
        OBSDataAutoRelease data = obs_data_create_from_json(saved);
        if (data)
            bindings = obs_data_get_array(data, "bindings");
    }

    // A null array clears the bindings, so another profile's don't carry over.
    obs_hotkey_load(id, bindings);
}
