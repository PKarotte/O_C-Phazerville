#pragma once

#include "../src/Audio/filter_variable2.h"
#include "../AudioIO.h"
#include <FS.h>
#include "../HSUtils.h"
#include "../HemisphereAudioApplet.h"
#include "../dsputils_arm.h"
#include "../Audio/AudioMixer.h"
#include "../Audio/AudioPassthrough.h"
#include "../Audio/AudioVCA.h"
#include "../Audio/InterpolatingStream.h"

// Include Braids BEFORE the macro rewrite so it never collides
// #include "VoiceApplet.h"
#include "BraidsVoiceApplet.h"
// #include "CoralVoiceApplet.h"

// hacks to effectively rewrite part of the applet boilerplate,
// making names and icons static
#define applet_name applet_name() const final { return applet_name_(); } \
  static constexpr const char* applet_name_

#define applet_icon applet_icon() const final { return applet_icon_(); } \
  static constexpr const uint8_t* applet_icon_

// ============================================================================
// Active Curated Audio Applets
// ============================================================================
#include "InputApplet.h"
#include "PassthruApplet.h"
#include "WAVPlayerApplet.h"
#include "LadderApplet.h"
#include "CloudverbApplet.h"
#include "LimiterApplet.h"

// ============================================================================
// Optional / Stock Phazerville Audio Applets
// (Uncomment here and in the registries below to include in custom builds)
// ============================================================================
// #include "CrosspanApplet.h"
// #include "DelayApplet.h"
// #include "DynamicsApplet.h"
// #include "FilterFolderApplet.h"
// #include "MidSideApplet.h"
// #include "OscApplet.h"
// #include "UpsampledApplet.h"
// #include "VCAApplet.h"
// #include "OneShotPlayerApplet.h"
// #include "HandSawApplet.h"
// #include "FreeverbApplet.h"
// #include "SamverbApplet.h"
// #include "PhaserApplet.h"
// #include "ThreeBandz.h"
// #include "TuneTrackerApplet.h"
// #include "FMDrumApplet.h"
// #include "GlitchApplet.h"
// #include "GritApplet.h"
// #include "MistierApplet.h"
// #include "AdvKrpsStrngApplet.h"
// #include "ModalResonatorApplet.h"
// #include "WAVRecorderApplet.h"
// #include "WTVCOApplet.h"
// #include "HarmOscApplet.h"
// #include "AbyssApplet.h"
// #include "AnimorfApplet.h"

#undef applet_name
#undef applet_icon

const size_t NUM_SLOTS = 5;

// Uncomment these factories if enabling SamverbApplet or DynamicsApplet:
// Factory<AudioEffectReverbSchroeder, 8> HemisphereAudioApplet::bung_factory;
// Factory<AudioEffectFreeverb, 8> HemisphereAudioApplet::verb_factory;
// Factory<AudioEffectDynamics, 20> HemisphereAudioApplet::compressor_factory;

// ============================================================================
// Mono Audio Applets Registry
// ============================================================================
constexpr Registry mono_applets = Registry<HemisphereAudioApplet, NUM_SLOTS * 2
    , DeclareFancyApplet<PassthruApplet<MONO>>
    , DeclareFancyApplet<InputApplet<MONO>>
    // , DeclareFancyApplet<VoiceApplet>
    , DeclareFancyApplet<BraidsVoiceApplet>
    // , DeclareFancyApplet<CoralVoiceApplet>
    , DeclareFancyApplet<WavPlayerApplet<MONO>>
    , DeclareFancyApplet<LadderApplet<MONO>>
    , DeclareFancyApplet<CloudverbApplet<MONO>>
    , DeclareFancyApplet<LimiterApplet<MONO>>

    // --- Optional Mono Applets (Uncomment to enable in custom builds) ---
    // , DeclareFancyApplet<UpsampledApplet<MONO>>
    // , DeclareFancyApplet<OscApplet>
    // , DeclareFancyApplet<HandSawApplet>
    // , DeclareFancyApplet<HarmOscApplet>
    // , DeclareFancyApplet<FMDrumApplet>
    // , DeclareFancyApplet<OneShotPlayerApplet<MONO>>
    // , DeclareFancyApplet<VcaApplet<MONO>>
    // , DeclareFancyApplet<FilterFolderApplet<MONO>>
    // , DeclareFancyApplet<DelayApplet<MONO>>
    // , DeclareFancyApplet<AbyssApplet<STEREO>>
    // , DeclareFancyApplet<AnimorfApplet<MONO>>
    // , DeclareFancyApplet<PhazerApplet>
    // , DeclareFancyApplet<BungverbApplet>
    // , DeclareFancyApplet<DynamicsApplet<MONO>>
    // , DeclareFancyApplet<ThreeBandzApplet<MONO>>
    // , DeclareFancyApplet<TuneTrackerApplet<MONO>>
    // , DeclareFancyApplet<GlitchApplet<MONO>>
    // , DeclareFancyApplet<GritApplet<MONO>>
    // , DeclareFancyApplet<MistierApplet<MONO>>
    // , DeclareFancyApplet<AdvKrpsStrngApplet>
    // , DeclareFancyApplet<WTVCOApplet>
    // , DeclareFancyApplet<ModalResonatorApplet<MONO>>
    // , DeclareFancyApplet<WavRecorderApplet<MONO>>
>{};

// ============================================================================
// Stereo Audio Applets Registry
// ============================================================================
constexpr Registry stereo_applets = Registry<HemisphereAudioApplet, NUM_SLOTS
    , DeclareFancyApplet<PassthruApplet<STEREO>>
    , DeclareFancyApplet<InputApplet<STEREO>>
    , DeclareFancyApplet<WavPlayerApplet<STEREO>>
    , DeclareFancyApplet<CloudverbApplet<STEREO>>
    , DeclareFancyApplet<LimiterApplet<STEREO>>

    // --- Optional Stereo Applets (Uncomment to enable in custom builds) ---
    // , DeclareFancyApplet<UpsampledApplet<STEREO>>
    // , DeclareFancyApplet<CrosspanApplet>
    // , DeclareFancyApplet<MidSideApplet>
    // , DeclareFancyApplet<VcaApplet<STEREO>>
    // , DeclareFancyApplet<DynamicsApplet<STEREO>>
    // , DeclareFancyApplet<ThreeBandzApplet<STEREO>>
    // , DeclareFancyApplet<DelayApplet<STEREO>>
    // , DeclareFancyApplet<AbyssApplet<STEREO>>
    // , DeclareFancyApplet<AnimorfApplet<STEREO>>
    // , DeclareFancyApplet<LadderApplet<STEREO>>
    // , DeclareFancyApplet<FilterFolderApplet<STEREO>>
    // , DeclareFancyApplet<OneShotPlayerApplet<STEREO>>
    // , DeclareFancyApplet<ModalResonatorApplet<STEREO>>
    // , DeclareFancyApplet<WavRecorderApplet<STEREO>>
>{};

static constexpr auto mono_appletIds = mono_applets.getIds();
constexpr int MONO_POOL_SIZE = mono_appletIds.size();

static constexpr auto stereo_appletIds = stereo_applets.getIds();
constexpr int STEREO_POOL_SIZE = stereo_appletIds.size();

template <typename ArrayType>
using Unwrap = typename std::remove_reference<
  typename std::remove_extent<ArrayType>::type>::type;

#include "../AudioAppletSubapp.h"

AudioAppletSubapp<NUM_SLOTS, MONO_POOL_SIZE, STEREO_POOL_SIZE, decltype(mono_applets), decltype(stereo_applets)> audio_app(
  mono_applets, stereo_applets
);
