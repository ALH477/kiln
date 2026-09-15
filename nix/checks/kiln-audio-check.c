/* SPDX-License-Identifier: MIT
 *
 * kiln_audio's music state and output tap, asserted on the host with the real
 * kiln_audio.c over plat/host's mixer bookkeeping.
 *
 *   playing   kiln_music_playing follows the XM PLAYER. libdragon's xm64.c calls
 *             mixer_ch_stop on every channel that has no sample on a tick, so
 *             the old test — "is the first mixer channel playing?" — read a
 *             resting first column as a stopped song. The check does exactly
 *             what xm64.c does to channel `first` and asks again.
 *   channels  kiln_music_first_channel is the first music channel while
 *             playing and -1 after a stop.
 *   tell      an XM handle reports a position; a bad handle reports -1.
 *   tap       kiln_audio_update hands every buffer it mixes to the tap, at the
 *             device's buffer length, and stops once the tap is removed.
 */
#include <kiln_audio.h>
#include <kiln_sound.h>
#include <libdragon.h>

#include <stdio.h>

/* plat/host/src/host_internal.h: credits the modelled device one presented
 * frame's worth of samples, which is what the launcher does after a present. */
void kiln_host_audio_frame(void);

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int g_taps, g_frames, g_bad_len;
static void tap(const int16_t *samples, int frames, void *ctx)
{
    (void)samples;
    CHECK(ctx == &g_taps, "tap: ctx not passed through");
    g_taps++;
    g_frames += frames;
    if (frames != audio_get_buffer_length()) g_bad_len++;
}

/* Present frames until the channel stops, counting mixed buffers. 400 is a
 * ceiling well past any sane length: a sample playing at 2 Hz never ends. */
static int buffers_until_silent(int ch)
{
    const int start = g_taps;
    for (int i = 0; i < 400 && kiln_sfx_playing(ch); i++) {
        kiln_host_audio_frame();
        kiln_audio_update();
    }
    return g_taps - start;
}

int main(void)
{
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_audio_init((KilnAudioConfig){
        .sample_rate = 32000, .latency = 0.16f, .sfx_channels = 4, .music_channels = 12,
    });

    const int h = kiln_music_load("rom:/music/tune.xm64");
    CHECK(h >= 0, "kiln_music_load returned %d", h);
    CHECK(!kiln_music_playing(h), "a loaded, unplayed track reports playing");
    CHECK(kiln_music_first_channel(h) == -1, "unplayed track has first channel %d",
          kiln_music_first_channel(h));

    kiln_music_play(h);
    const int first = kiln_music_first_channel(h);
    CHECK(first == 4, "first music channel is %d, expected 4 (after 4 SFX)", first);
    CHECK(kiln_music_playing(h), "a playing track reports stopped");

    /* xm64.c's sync loop, for a column with no note this tick. */
    mixer_ch_stop(first);
    CHECK(kiln_music_playing(h),
          "the track reads as STOPPED because its first channel rests - "
          "kiln_music_playing is asking a mixer channel, not the player");

    int pat = 99, row = 99;
    float secs = -1.0f;
    kiln_music_tell(h, &pat, &row, &secs);
    CHECK(pat == 0 && row == 0 && secs == 0.0f, "tell at start: pattern %d row %d secs %f",
          pat, row, (double)secs);
    kiln_music_tell(h + 7, &pat, &row, NULL);
    CHECK(pat == -1 && row == -1, "tell on a bad handle: pattern %d row %d", pat, row);
    kiln_music_seek(h, 0, 0);           /* must not disturb anything */
    CHECK(kiln_music_playing(h), "seek stopped the track");

    kiln_music_stop(h);
    CHECK(!kiln_music_playing(h), "a stopped track reports playing");
    CHECK(kiln_music_first_channel(h) == -1, "stopped track keeps first channel %d",
          kiln_music_first_channel(h));

    kiln_audio_set_tap(tap, &g_taps);
    kiln_audio_update();
    CHECK(g_taps > 0, "kiln_audio_update mixed buffers but the tap saw none");
    CHECK(g_bad_len == 0, "%d tapped buffers were not audio_get_buffer_length() long", g_bad_len);
    CHECK(g_frames == g_taps * audio_get_buffer_length(), "tap frames %d over %d buffers",
          g_frames, g_taps);
    printf("  tap: %d buffers, %d frames\n", g_taps, g_frames);

    kiln_audio_set_tap(NULL, NULL);
    const int before = g_taps;
    kiln_host_audio_frame();
    kiln_host_audio_frame();
    kiln_audio_update();
    CHECK(g_taps == before, "a removed tap was still called");

    /* ── pitch ── blip.wav64 is 5512 frames at 22050 Hz: at a 32 kHz output
     * that is 8000 output frames, ~16 of the host's 512-frame buffers, and an
     * octave up is half that. Both ratios exceed the default limit (the output
     * rate) above 1.45x, so the limits go up first — the console asserts
     * otherwise, and now so does the host. */
    for (int ch = 0; ch < 4; ch++) mixer_ch_set_limits(ch, 16, 64000.0f, 0);
    const int blip = kiln_sfx_load("rom:/sfx/blip.wav64");
    kiln_audio_set_tap(tap, &g_taps);

    kiln_sfx_play(blip, 0, 1);
    const int n1 = buffers_until_silent(0);
    kiln_sfx_play(blip, 0, 1);
    kiln_sfx_set_pitch(0, 2.0f);
    const int n2 = buffers_until_silent(0);
    printf("  pitch: 1.0 -> %d buffers, 2.0 -> %d buffers\n", n1, n2);
    CHECK(n1 >= 14 && n1 <= 18, "blip at pitch 1 lasted %d buffers, expected ~16", n1);
    CHECK(n2 * 2 >= n1 - 2 && n2 * 2 <= n1 + 2,
          "kiln_sfx_set_pitch(2.0) lasted %d buffers against %d at 1.0 - not an octave", n2, n1);

    kiln_sound_init(&(KilnSoundShader){ .name = "blip", .wav64_path = "rom:/sfx/blip.wav64",
                                        .base_vol = 1.0f }, 1);
    const int sch = kiln_sound_play("blip", (fm_vec3_t){{ 0, 0, 0 }}, 2.0f);
    CHECK(sch >= 0 && sch < 4, "kiln_sound_play got channel %d", sch);
    const int n3 = sch >= 0 ? buffers_until_silent(sch) : 0;
    printf("  kiln_sound_play pitch 2.0 -> %d buffers\n", n3);
    CHECK(n3 >= n2 - 1 && n3 <= n2 + 1,
          "kiln_sound_play(pitch 2.0) lasted %d buffers, kiln_sfx_set_pitch(2.0) %d - "
          "kiln_sound is passing its ratio as Hz", n3, n2);

    /* ── stereo ── a stereo wav64 plays on ch and makes ch+1 its secondary;
     * libdragon asserts on any play, set_vol or set_freq through a secondary,
     * and the host now does too. Fill the four SFX channels with two stereo
     * voices, then ask for more: the steal must take a whole PAIR from an
     * owner, never a secondary (priority 0, because nothing was played on it
     * directly — which is exactly what the old allocator chose). */
    const int st = kiln_sfx_load("rom:/stereo.wav64");
    for (int ch = 0; ch < 4; ch++) kiln_sfx_stop(ch);
    const int s0 = kiln_sfx_play(st, -1, 1);
    const int s1 = kiln_sfx_play(st, -1, 1);
    printf("  stereo: first pair at %d, second at %d\n", s0, s1);
    CHECK(s0 == 0 && s1 == 2, "two stereo voices landed on %d and %d, expected pairs 0 and 2", s0, s1);
    CHECK(kiln_sfx_playing(1) && kiln_sfx_playing(3), "a secondary does not report its owner playing");

    const int s2 = kiln_sfx_play(st, -1, 2);   /* steals a pair */
    CHECK(s2 == 0 || s2 == 2, "a stereo steal landed on channel %d, not an owner", s2);
    const int m0 = kiln_sfx_play(blip, -1, 3); /* mono, steals an owner */
    CHECK(m0 == 0 || m0 == 2, "a mono steal landed on channel %d, a secondary", m0);
    kiln_sfx_set_vol_pan(m0 + 1, 0.5f, 0.5f);  /* through whatever is on m0+1 */
    kiln_sfx_set_pitch(m0 + 1, 1.0f);

    for (int ch = 0; ch < 4; ch++) kiln_sfx_stop(ch);
    const int s3 = kiln_sfx_play(st, 1, 1);    /* explicit: pair 1..2 */
    CHECK(s3 == 1, "explicit stereo on channel 1 returned %d", s3);
    const int m1 = kiln_sfx_play(blip, 2, 1);  /* explicit onto the secondary */
    CHECK(m1 == 2 && !kiln_sfx_playing(1),
          "an explicit play onto a secondary returned %d with its owner still playing %d",
          m1, kiln_sfx_playing(1));
    CHECK(kiln_sfx_play(st, 3, 1) == -1, "a stereo pair starting on the last SFX channel was accepted");

    if (fails) { printf("kiln-audio: %d failure(s)\n", fails); return 1; }
    printf("kiln-audio: ok\n");
    return 0;
}
