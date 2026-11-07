/*
 * WAV file player applet
 *     by djphazer
 *     based on TeensyVariablePlayback library by Nic Newdigate
 *
 * It looks in the root directory of the SD card for the
 * selected file, by number, in the format:
 *   000.WAV
 *   001.WAV
 *   002.WAV
 *   ...
 *
 * All sampling rates should work, but they must be 16-bit.
 *
 * Sync mode will automatically lock tempo with the internal clock.
 *
 * Use AuxButton (X or Y) for Play/Stop.
 *
 */

#include <TeensyVariablePlayback.h>

template <AudioChannels Channels>
class WavPlayerApplet : public HemisphereAudioApplet {
public:
  const uint64_t applet_id() override {
    return strhash("WavPlay");
  }
  const char* applet_name() {
    return "WavPlay";
  }

  void Start() {
    for (int i = 0; i < Channels; i++) {
      PatchCable(input, i, mixer[i], 3);
      mixer[i].gain(3, 1.0);
      PatchCable(mixer[i], 0, output, i);
    }

    PatchCable(wavplayer, 0, hpfilter[0], 0);
    PatchCable(wavplayer, 1, hpfilter[1], 0);
    PatchCable(wavplayer, 0, mixer[0], 0);
    PatchCable(wavplayer, 1, mixer[1], 0);
    PatchCable(hpfilter[0], 2, mixer[0], 1);
    PatchCable(hpfilter[1], 2, mixer[1], 1);
    PatchCable(hpfilter[0], 0, mixer[0], 2);
    PatchCable(hpfilter[1], 0, mixer[1], 2);

    hpfilter[0].resonance(1.0);
    hpfilter[1].resonance(1.0);
    SetFilter(0);

    if (!SDcard_Ready) {
      Serial.println("Unable to access the SD card");
    }
    else {
      wavplayer.enableInterpolation(true);
      wavplayer.setBufferInPSRAM(false);
    }
  }

  void Unload() {
    wavplayer.stop();
    AllowRestart();
  }

  void Reset() override {
    playstop_cv.Reset();
    clock_was_running = false;
  }

  void Controller() {
    djfilter_mod = constrain(djfilter + ReadModulation(djfilter_cv, 128), -64, 64);
    SetFilter(filter_on * djfilter_mod);

    float gain_mod = (float)ReadModulation(level_cv, 100) * 0.01f;
    float gain = dbToScalar(level) + gain_mod;
    if (gain < 0.0f) gain = 0.0f;

    FileLevel(gain);
    if (tempo_sync) {
      FileMatchTempo();
    } else {
      float rate_mod = (float)ReadModulation(playrate_cv, 100) * 0.01f;
      FileRate(0.01f * playrate + rate_mod);
    }

    start_beat_mod = constrain(start_beat + start_beat_cv.SemitoneIn(), 0, 999);
    if (wavplayer_ready)
      wavplayer.setBeatStart(start_beat_mod);

    int beats_mod = ReadModulation(loop_len_beats_cv, 131);
    effective_loop_length = constrain(loop_length + beats_mod, 0, 131);

    UpdateLoopPoints();

    bool clock_running = HS::clock_m.IsRunning();
    if (loop_on) {
      if (clock_running && !clock_was_running) {
        if (!FileIsPlaying()) {
          StartPlaying();
        } else {
          retrig = true;
        }
      } else if (!clock_running && clock_was_running) {
        if (FileIsPlaying()) {
          wavplayer.stop();
        }
      }
    }
    clock_was_running = clock_running;

    if (FileIsPlaying()) {
      if (loop_on && finish_ms > start_ms && wavplayer.positionMillis() >= finish_ms) {
        retrig = true;
      }

      if (loop_on && effective_loop_length < 4 && clock_running) {
        uint32_t beat_duration_ms = 500;
        float tempo = HS::clock_m.GetTempoFloat();
        if (tempo > 10.0f) {
          beat_duration_ms = (uint32_t)(60000.0f / tempo);
        }

        uint32_t sub_duration_ms = beat_duration_ms;
        switch (effective_loop_length) {
          case 0: sub_duration_ms = beat_duration_ms / 16; break;
          case 1: sub_duration_ms = beat_duration_ms / 8;  break;
          case 2: sub_duration_ms = beat_duration_ms / 4;  break;
          case 3: sub_duration_ms = beat_duration_ms / 2;  break;
        }

        if (sub_duration_ms < 5) sub_duration_ms = 5;

        uint32_t now = millis();
        if (now - last_subbeat_ms >= sub_duration_ms) {
          last_subbeat_ms = now;
          retrig = true;
          sync_trig = true;
        }
      }

      if (HS::clock_m.EndOfBeat()) {
        if (loop_on && effective_loop_length >= 4) {
          int target_beats = effective_loop_length - 3;
          if (++loop_count >= target_beats)
            retrig = true;
        }
        sync_trig = true;
      }
    }

    if (playstop_cv.Clock()) {
      if (FileIsPlaying())
        retrig = true;
      else
        StartPlaying();
    }

    titlestat[7] = FileIsPlaying() ? '*' : ' ';
    titlestat[8] = (filter_on) ? '/' : ' ';

    if (!HS::clock_m.IsRunning() || HS::clock_m.EndOfBeat())
      go_time = true;
  }

  void mainloop() {
    if (SDcard_Ready) {
      if (wavplayer_reload) {
        FileLoad();
        wavplayer_reload = false;
      }

      if (wavplayer_playtrig && (go_time || !tempo_sync)) {
        ApplyLoopPositions();
        wavplayer.play();
        loop_count = 0;
        last_subbeat_ms = millis();
        wavplayer_playtrig = false;
      }

      if (syncloopstart && go_time) {
        ToggleLoop();
        syncloopstart = false;
        retrig = false;
      }

      if (retrig) {
        if (wavplayer.available()) {
          ApplyLoopPositions();
          wavplayer.retrigger();
          loop_count = 0;
        }
        retrig = false;
      }

      if (tempo_sync && sync_trig) {
        wavplayer.syncTrig();
        sync_trig = false;
      }
    }
  }

  void View() {
    if (!SDcard_Ready) {
      gfxPrint(4, 25, "NO SD CARD!!");
      return;
    }
    size_t y = 13;
    gfxStartCursor(1, y);
    gfxPrintfn(1, y, 0, "%03u", GetFileNum());
    gfxEndCursor(cursor == FILE_NUM, "FileSel");

    gfxIcon(22, y, FileIsPlaying() ? PLAY_ICON : STOP_ICON);
    gfxStartCursor(30, y-1);
    gfxPrint(playstop_cv);
    gfxEndCursor(cursor == PLAYSTOP_GATE_CV, false, playstop_cv.InputName(), "PlayTrig");

    if (wavplayer_ready)
      gfxPrint(37, y, GetFileBPM());
    else
      gfxPrint(37, y, "(--)");

    gfxStartCursor(56, y);
    gfxPrint(djfilter_cv);
    gfxEndCursor(cursor == FILTER_CV, false, djfilter_cv.InputName(), "FilterCV");

    if (filter_on) {
      const int w = abs(djfilter_mod);
      const int x = (djfilter_mod < 0) ? (63 - w) : 0;
      gfxInvert(x, y-3, w, 4);
    }

    y += 10;
    if (FileIsPlaying()) {
      uint32_t tmilli = GetFileTime();
      uint32_t tsec = tmilli / 1000;
      uint32_t tmin = tsec / 60;
      tmilli %= 1000;
      tsec %= 60;

      gfxPos(1, y);
      graphics.printf("%02lu:%02lu.%03lu", tmin, tsec, tmilli);
    }
    if (cursor == FILTER_PARAM) {
      if (EditMode()) {
        SetLabel("Filter");
        const int x = 45;
        gfxClear(x, y, 18, 10);
        if (djfilter > 0) gfxPrint(x, y, "HPF");
        else if (djfilter < 0) gfxPrint(x, y, "LPF");
        else gfxPrint(x+6, y, "X");
      } else {
        gfxIcon(49, y, RIGHT_ICON, true);
        gfxIcon(56, y, (filter_on) ? SLEW_ICON : PhzIcons::tuner, true);
      }
    }

    y += 10;
    gfxStartCursor(1, y);
    graphics.printf("%3ddB", level);
    gfxEndCursor(cursor == LEVEL, "Gain");
    gfxStartCursor();
    gfxPrint(level_cv);
    gfxEndCursor(cursor == LEVEL_CV, false, level_cv.InputName(), "Gain CV");

    y += 10;
    if (tempo_sync) {
      gfxPrint(1, y, "Sync:");
    } else {
      gfxPrint(1, y, "Rate:");
    }
    gfxStartCursor();
    graphics.printf("%3d%%", playrate);
    gfxEndCursor(cursor == PLAYRATE, true);
    gfxStartCursor();
    gfxPrint(playrate_cv);
    gfxEndCursor(cursor == PLAYRATE_CV, false, playrate_cv.InputName(), "Rate CV");

    y += 10;
    if (cursor < LOOP_START_PCT) {
      gfxIcon(1, y, PULSES_ICON);
      gfxStartCursor(11, y);
      graphics.printf("%3u", ((cursor==START_BEAT && EditMode()) ? start_beat : start_beat_mod) + 1);
      gfxEndCursor(cursor == START_BEAT, "StartBeat");
      if (start_beat_mod != start_beat) gfxIcon(30, y, CV_ICON);

      gfxStartCursor(40, y);
      gfxPrint(start_beat_cv);
      gfxEndCursor(cursor == START_BEAT_CV, false, start_beat_cv.InputName(), "Start CV");
    } else if (cursor <= LOOP_LEN_CV) {
      gfxPrint(1, y, "St:");
      gfxStartCursor(17, y);
      graphics.printf("%2d%%", loop_start_mod);
      gfxEndCursor(cursor == LOOP_START_PCT, "LoopSt%");
      gfxStartCursor(37, y);
      gfxPrint(loop_start_cv);
      gfxEndCursor(cursor == LOOP_START_CV, false, loop_start_cv.InputName(), "Strt CV");

      gfxPrint(43, y, "L:");
      gfxStartCursor(52, y);
      graphics.printf("%2d", loop_len_mod);
      gfxEndCursor(cursor == LOOP_LEN_PCT, "LoopLn%");
    } else {
      gfxIcon(1, y, LOOP_ICON);
      gfxStartCursor(10, y);
      uint8_t disp_val = (cursor == LOOP_LENGTH && EditMode()) ? loop_length : effective_loop_length;
      switch (disp_val) {
        case 0: gfxPrint("1/16"); break;
        case 1: gfxPrint("1/8");  break;
        case 2: gfxPrint("1/4");  break;
        case 3: gfxPrint("1/2");  break;
        default:
          graphics.printf("%3u", disp_val - 3);
          break;
      }
      gfxEndCursor(cursor == LOOP_LENGTH, "LoopBeats");

      gfxStartCursor(28, y);
      gfxPrint(loop_len_beats_cv);
      gfxEndCursor(cursor == LOOP_BEATS_CV, false, loop_len_beats_cv.InputName(), "BeatsCV");

      gfxIcon(50, y, loop_on ? CHECK_ON_ICON : CHECK_OFF_ICON);
      if (cursor == LOOP_ENABLE)
        gfxIcon(42, y, RIGHT_ICON, true);
    }

    gfxDisplayInputMapEditor();
  }

  void AuxButton() {
    if (FILTER_PARAM == cursor) {
      filter_on = !filter_on;
    } else if (PLAYRATE == cursor) {
      tempo_sync ^= 1;
    } else
      ToggleFilePlayer();
  }

  void OnButtonPress() {
    if (CheckEditInputMapPress(
          cursor,
          IndexedInput(PLAYSTOP_GATE_CV, playstop_cv),
          IndexedInput(FILTER_CV, djfilter_cv),
          IndexedInput(LEVEL_CV, level_cv),
          IndexedInput(PLAYRATE_CV, playrate_cv),
          IndexedInput(START_BEAT_CV, start_beat_cv),
          IndexedInput(LOOP_START_CV, loop_start_cv),
          IndexedInput(LOOP_LEN_CV, loop_len_cv),
          IndexedInput(LOOP_BEATS_CV, loop_len_beats_cv)
    )) return;

    if (LOOP_ENABLE == cursor) {
      syncloopstart = true;
      go_time = false;
    } else
      CursorToggle();
  }

  void OnEncoderMove(int direction) {
    if (!EditMode()) {
      MoveCursor(cursor, direction, NUM_PARAMS - 1);
      return;
    }
    if (EditSelectedInputMap(direction)) return;
    switch (cursor) {
      case FILE_NUM:
        ChangeToFile(GetFileNum() + direction);
        break;
      case PLAYSTOP_GATE_CV:
        playstop_cv.ChangeSource(direction);
        break;
      case FILTER_PARAM:
        filter_on = true;
        djfilter = constrain(djfilter + direction, -63, 63);
        break;
      case FILTER_CV:
        djfilter_cv.ChangeSource(direction);
        break;
      case LEVEL:
        level = constrain(level + direction, -90, 90);
        break;
      case LEVEL_CV:
        level_cv.ChangeSource(direction);
        break;
      case PLAYRATE:
        playrate = constrain(playrate + direction, -200, 200);
        break;
      case PLAYRATE_CV:
        playrate_cv.ChangeSource(direction);
        break;
      case START_BEAT:
        start_beat = constrain(start_beat + direction, 0, 999);
        break;
      case START_BEAT_CV:
        start_beat_cv.ChangeSource(direction);
        break;
      case LOOP_START_PCT:
        loop_start_pct = constrain(loop_start_pct + direction, 0, 99);
        break;
      case LOOP_START_CV:
        loop_start_cv.ChangeSource(direction);
        break;
      case LOOP_LEN_PCT:
        loop_len_pct = constrain(loop_len_pct + direction, 1, 100);
        break;
      case LOOP_LEN_CV:
        loop_len_cv.ChangeSource(direction);
        break;
      case LOOP_LENGTH:
        loop_length = constrain(loop_length + direction, 0, 131);
        break;
      case LOOP_BEATS_CV:
        loop_len_beats_cv.ChangeSource(direction);
        break;
    }
  }

  void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
    wavplayer.stop();
    uint8_t oldfilenum = 0;
    data[0] = PackPackables(level, level_cv, uint8_t(tempo_sync), playrate_cv, oldfilenum, djfilter);
    data[1] = PackPackables(playrate, djfilter_cv, loop_start_pct, loop_length, loop_len_beats_cv);
    data[2] = PackPackables(start_beat, start_beat_cv, playstop_cv);
    data[3] = PackPackables(wavplayer_select, loop_len_pct, loop_start_cv, loop_len_cv);
  }

  void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
    int8_t old_playrate;
    uint8_t oldfilenum;
    UnpackPackables(data[0], level, level_cv, old_playrate, playrate_cv, oldfilenum, djfilter);
    UnpackPackables(data[1], playrate, djfilter_cv, loop_start_pct, loop_length, loop_len_beats_cv);
    UnpackPackables(data[2], start_beat, start_beat_cv, playstop_cv);
    UnpackPackables(data[3], wavplayer_select, loop_len_pct, loop_start_cv, loop_len_cv);
    if (playrate == 0) playrate = old_playrate;
    if (old_playrate) tempo_sync = true;
    if (loop_length == 0) loop_length = 11;
    if (loop_len_pct == 0) loop_len_pct = 100;

    ChangeToFile(oldfilenum ? oldfilenum : wavplayer_select);
  }

  AudioStream* InputStream() override {
    return &input;
  }
  AudioStream* OutputStream() override {
    return &output;
  }

protected:
  void SetHelp() override {
    help[HELP_DIGITAL1] = "PlayTrg";
    help[HELP_DIGITAL2] = "LoopTog";
    help[HELP_CV1]      = "StrtMod";
    help[HELP_CV2]      = "LenMod";
    help[HELP_OUT1]     = "AudioL";
    help[HELP_OUT2]     = "AudioR";
  }

private:
  enum WAVCursor {
    FILE_NUM,
    PLAYSTOP_GATE_CV,
    FILTER_PARAM,
    FILTER_CV,
    LEVEL,
    LEVEL_CV,
    PLAYRATE,
    PLAYRATE_CV,
    START_BEAT,
    START_BEAT_CV,
    LOOP_START_PCT,
    LOOP_START_CV,
    LOOP_LEN_PCT,
    LOOP_LEN_CV,
    LOOP_LENGTH,
    LOOP_BEATS_CV,
    LOOP_ENABLE,

    NUM_PARAMS
  };

  char titlestat[10] = "WavPlay  ";

  int cursor = 0;
  int8_t level = -3;
  int8_t djfilter = 0;
  int8_t djfilter_mod = 0;
  CVInputMap djfilter_cv;

  bool lowcut = false;
  bool filter_on = false;
  bool tempo_sync = true;

  DigitalInputMap playstop_cv;
  CVInputMap level_cv;
  int16_t playrate = 100;
  CVInputMap playrate_cv;

  bool go_time = false;
  bool sync_trig = false;
  bool retrig = false;

  AudioPassthrough<Channels> input;
  AudioPlaySdResmp      wavplayer;
  AudioFilterStateVariable2 hpfilter[2];
  AudioMixer4           mixer[2];
  AudioPassthrough<Channels> output;

  bool wavplayer_reload = true;
  bool wavplayer_playtrig = false;
  bool wavplayer_ready = false;
  uint16_t wavplayer_select = 1;
  uint16_t start_beat = 0;
  uint16_t start_beat_mod = 0;
  uint8_t loop_length = 11;
  uint8_t effective_loop_length = 11;
  int8_t loop_count = 0;
  bool loop_on = true;
  bool syncloopstart = false;

  bool clock_was_running = false;

  CVInputMap start_beat_cv;

  int8_t loop_start_pct = 0;
  int8_t loop_start_mod = 0;
  CVInputMap loop_start_cv;

  int8_t loop_len_pct = 100;
  int8_t loop_len_mod = 100;
  CVInputMap loop_len_cv;
  CVInputMap loop_len_beats_cv;

  uint32_t start_ms = 0;
  uint32_t finish_ms = 0;
  uint32_t start_byte_pos = 0;
  uint32_t finish_byte_pos = 0;

  uint32_t last_subbeat_ms = 0;

  int ReadModulation(CVInputMap &map, int scale) {
    return map.InRescaled(scale);
  }

  void UpdateLoopPoints() {
    uint32_t total_milli = wavplayer.lengthMillis();
    if (total_milli == 0) return;

    int st_cv = ReadModulation(loop_start_cv, 100);
    loop_start_mod = constrain(loop_start_pct + st_cv, 0, 99);

    int len_cv = ReadModulation(loop_len_cv, 100);
    int max_avail = 100 - loop_start_mod;
    loop_len_mod = constrain(loop_len_pct + len_cv, 1, max_avail);

    start_ms = ((uint64_t)total_milli * loop_start_mod) / 100;
    finish_ms = start_ms + (((uint64_t)total_milli * loop_len_mod) / 100);
    if (finish_ms > total_milli) finish_ms = total_milli;

    start_byte_pos = ((uint64_t)start_ms * 44100 * 4) / 1000;
    start_byte_pos &= ~0x03;

    finish_byte_pos = ((uint64_t)finish_ms * 44100 * 4) / 1000;
    finish_byte_pos &= ~0x03;
  }

  void ApplyLoopPositions() {
    UpdateLoopPoints();
    wavplayer.setLoopStart(start_byte_pos);
    wavplayer.setLoopFinish(finish_byte_pos);
    wavplayer.setPlayStart(loop_on ? play_start_loop : play_start_sample, start_byte_pos);
  }

  void FileLoad() {
    char filename[] = "000.WAV";
    filename[0] += wavplayer_select / 100;
    filename[1] += wavplayer_select / 10 % 10;
    filename[2] += wavplayer_select % 10;
    wavplayer_ready = wavplayer.playWav(filename);
    if (wavplayer_ready) {
      UpdateLoopPoints();
    }
  }

  void StartPlaying() {
    ApplyLoopPositions();
    wavplayer_playtrig = true;
    go_time = false;
  }

  bool FileIsPlaying() {
    return wavplayer.isPlaying();
  }

  void ToggleFilePlayer() {
    if (wavplayer.isPlaying()) {
      wavplayer.stop();
    } else if (SDcard_Ready) {
      StartPlaying();
    }
  }

  void ToggleLoop() {
    loop_on = !loop_on;
    if (loop_on) {
      loop_count = 0;
      last_subbeat_ms = millis();
      ApplyLoopPositions();
      if (HS::clock_m.IsRunning()) {
        if (wavplayer.available()) {
          wavplayer.retrigger();
        } else {
          StartPlaying();
        }
      }
    } else {
      if (FileIsPlaying()) {
        wavplayer.setPlayStart(play_start_sample, start_byte_pos);
      }
    }
  }

  static constexpr int FILTER_MAX = (60 << 7);
  void SetFilter(int scalar) {
    lowcut = (scalar < 0);
    if (scalar < 2 && scalar > -2) {
      for (int ch = 0; ch < Channels; ++ch) {
        hpfilter[ch].frequency(0);
      }
      return;
    }

    float freq = scalar * FILTER_MAX / 64;
    if (lowcut)
      freq = (FILTER_MAX - abs(freq)) / 64;
    else
      freq = abs(freq) / 64;
    freq *= freq;

    for (int ch = 0; ch < Channels; ++ch) {
      hpfilter[ch].frequency(freq);
    }
  }

  void ChangeToFile(int select) {
    wavplayer_select = (uint16_t)constrain(select, 0, 999);
    wavplayer_reload = true;
    if (wavplayer.isPlaying()) {
      StartPlaying();
    }
  }

  uint16_t GetFileNum() {
    return wavplayer_select;
  }

  uint32_t GetFileTime() {
    return wavplayer.positionMillis();
  }

  uint16_t GetFileBPM() {
    return (uint16_t)wavplayer.getBPM();
  }

  void FileMatchTempo() {
    wavplayer.matchTempo(
      HS::clock_m.GetTempoFloat() * (playrate * 0.01f + ((float)ReadModulation(playrate_cv, 100) * 0.01f))
    );
  }

  void FileLevel(float lvl) {
    bool dry = (djfilter_mod < 2 && djfilter_mod > -2);
    for (int ch = 0; ch < Channels; ++ch) {
      mixer[ch].gain(0, lvl * dry);
      mixer[ch].gain(1, lvl * (1-lowcut) * (1-dry));
      mixer[ch].gain(2, lvl * lowcut * (1-dry));
    }
  }

  void FileRate(float rate) {
    wavplayer.setPlaybackRate(rate);
  }
};