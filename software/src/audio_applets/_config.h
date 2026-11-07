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

// Other applets that rely on the macro rewrite
#include "InputApplet.h"
#include "PassthruApplet.h"
#include "WAVPlayerApplet.h"
#include "LadderApplet.h"
#include "CloudverbApplet.h"
#include "LimiterApplet.h"


#undef applet_name
#undef applet_icon

const size_t NUM_SLOTS = 5;

// TODO: categories
constexpr Registry mono_applets = Registry<HemisphereAudioApplet, NUM_SLOTS * 2
    , DeclareFancyApplet<PassthruApplet<MONO>>
    , DeclareFancyApplet<InputApplet<MONO>>
    // , DeclareFancyApplet<VoiceApplet>
    , DeclareFancyApplet<BraidsVoiceApplet>
    , DeclareFancyApplet<WavPlayerApplet<MONO>>
    , DeclareFancyApplet<LadderApplet<MONO>>
    , DeclareFancyApplet<CloudverbApplet<MONO>>
    , DeclareFancyApplet<LimiterApplet<MONO>>
>{};

constexpr Registry stereo_applets = Registry<HemisphereAudioApplet, NUM_SLOTS
    , DeclareFancyApplet<PassthruApplet<STEREO>>
    , DeclareFancyApplet<InputApplet<STEREO>>
    , DeclareFancyApplet<WavPlayerApplet<STEREO>>
    , DeclareFancyApplet<CloudverbApplet<STEREO>>
    , DeclareFancyApplet<LimiterApplet<STEREO>>
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
