#pragma once

// Ariane version this build is based on (Dryxio/ariane). Kept for code that reports it.
#define ARIANE_VERSION "1.40.11"
#define ARIANE_BASE_VERSION ARIANE_VERSION

// Ariane Plus version — bump this when releasing Ariane Plus updates.
// Keep it monotonic for the updater: after 1.29, use 1.30, not 1.3.
#define ARIANE_PLUS_VERSION "1.0.0"

// GitHub repository the updater checks for Ariane Plus releases (tags "v<version>-<channel>")
#define ARIANE_PLUS_REPO "NikolasOG/ariane-plus"
#define ARIANE_WIDEN2(x) L##x
#define ARIANE_WIDEN(x) ARIANE_WIDEN2(x)
#define ARIANE_PLUS_REPO_W ARIANE_WIDEN(ARIANE_PLUS_REPO)

// Update channel — set at build time via preprocessor define
// Defaults to "master" if not set by the build system
#ifndef ARIANE_CHANNEL
#define ARIANE_CHANNEL "master"
#endif

// Display name for the channel (shown in title bar / update popup)
#define ARIANE_CHANNEL_DISPLAY (strcmp(ARIANE_CHANNEL, "PE") == 0 ? "FLA" : ARIANE_CHANNEL)
