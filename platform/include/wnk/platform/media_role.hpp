// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether this install's luna bus role lets LG's media player run. The role is written at install
// from the firmware's /usr/share/rolegen/templates/NDK.json. webOS 11.2's player registers a
// com.webos.media.smp.* name, which that template grants and a role written before the update lacks:
// then the player's config calls fail and NDL_DirectMediaLoad never returns, hanging in
// ~StarfishMediaAPIs (client-webos docs/NOTES.md, its ndl::ensure_media_role). Only a reinstall
// rewrites the role. So before a load: when the template names the player bus, try to register such a
// name, and refuse the load if the hub says no.
#pragma once

#include <string>

namespace wnk::platform {

// Probed once per process. False with *why ("... reinstall the app") when the role is stale; true
// when it is fine, when the template does not name the bus (older firmware), or off webOS.
bool media_role_ok(std::string *why = nullptr);

// Pure, for tests: whether a template's text grants the player bus prefix.
bool media_role_template_names_player_bus(const std::string &template_text);

}  // namespace wnk::platform
