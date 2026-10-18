// SPDX-License-Identifier: GPL-3.0-or-later
//
// What this television says about its own media hardware, for choosing a backend and what to declare.
//
// The decoder table PlxNative reads (/etc/umediaserver/device_codec_capability_config.json) is not
// visible inside the Developer Mode jail on webOS 6.5.3 (checked 2026-10-11: no /etc/umediaserver
// at all). LG's configuration service answers the app instead, in a few milliseconds, with
// model-level facts: HEVC support, panel resolution and rate, HDR, Dolby Vision and Atmos. It has no
// per-codec limits and no AV1/VP9 entry.
//
// Every answer is three-valued: a key the firmware does not report stays unknown and never reads as
// "no" or "yes".
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace wnk::media {

struct DeviceMediaCaps {
    bool measured = false;                 // a reply was parsed
    std::optional<bool> hevc;              // tv.conti.supportHevc
    std::optional<bool> uhd_panel;         // tv.hw.panelResolution == "UD" (or 8K)
    std::optional<int> panel_hz;           // tv.hw.panelOutputFrameRate ("120Hz")
    std::optional<bool> hdr;               // tv.model.supportHDR
    std::optional<bool> dolby_vision;      // tv.config.supportDolbyHDRContents (PlxNative's gate)
    std::optional<bool> dolby_atmos;       // tv.config.supportDolbyTVATMOS
    // tv.model.edidType ("TrueHD" on the OLED65C1PTB): Kodi treats a value naming "dts" as the TV
    // passing DTS to its pipeline (2023+ sets).
    std::string edid_audio;
    std::optional<bool> dts;
    std::string soc;                       // tv.hw.SoCChipType
    std::string detail;                    // why nothing was measured

    // The getConfigs request this module sends.
    static std::string request();
    // Parses a getConfigs reply; unmeasured (with detail) if it is not one.
    static DeviceMediaCaps parse(const std::string &reply);
    // Blocking Luna call (startup or a worker thread); unmeasured off webOS.
    static DeviceMediaCaps detect(int timeout_ms = 2000);

    // One line for logs: "hevc yes, uhd yes, 120 Hz, hdr yes, dv yes, atmos yes, dts no, soc O20N".
    std::string summary() const;
};

// The TV's sound output link and what the device on it takes, from
// com.webos.service.arccontroller/getARCState (the call Kodi reads). Measured on the OLED65C1PTB with
// an eARC soundbar (2026-10-19): arcActivation "eARC", earcCapability ["AAC","DTS","PCM","DD","DD+",
// "TrueHD","MAT2PCM"], earcATMOS true, PCM up to 8 channels. The reply over plain ARC was not seen, so
// `formats` is filled only for eARC. What the device takes is not what an app can send it: TrueHD
// and MAT stay out of reach (no app pipeline takes TrueHD, docs/research/AUDIO-PASSTHROUGH.md).
struct AudioOutputCaps {
    bool measured = false;
    std::string link;                  // arcActivation: "eARC", "ARC" or other/empty (no ARC device)
    std::vector<std::string> formats;  // earcCapability
    std::optional<bool> atmos;         // earcATMOS
    int pcm_channels = 0;              // advancedEarcCapability, PCM maxChannelNumber; 0 unknown
    std::string detail;                // why nothing was measured

    bool earc() const { return link == "eARC"; }
    bool takes(const std::string &format) const;
    static AudioOutputCaps parse(const std::string &reply);
    static AudioOutputCaps detect(int timeout_ms = 2000);
    // "eARC: AAC DTS PCM DD DD+ TrueHD MAT2PCM, atmos yes, pcm 8 ch".
    std::string summary() const;
};

}  // namespace wnk::media
