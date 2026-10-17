#include "sound.h"
#include "pcspkr.h"
#include "audio.h"

/* UI cues play as short synthesized PCM tones through whatever audio
 * backend Media Player uses (SB16/AC97) when one is available — the PC
 * speaker (port 0x61 + PIT channel 2) is only *emulated* by most
 * hypervisors and frequently isn't wired to the host's real speakers at
 * all (VirtualBox/VMware in particular), even though the emulation
 * itself works fine. Falls back to pcspkr_beep() when no sound card is
 * present, since that's still better than silence on real hardware
 * without SB16/AC97. */

static int enabled = 1;
static int audio_ready = 0;

static uint8_t snd_buf[AUDIO_DMA_SIZE];

static void ensure_audio(void)
{
    if (audio_ready) return;
    audio_ready = 1;
    if (!audio_available()) audio_init();
}

static int fill_tone(uint8_t *buf, int off, int cap, uint32_t freq, uint32_t ms)
{
    int n = (int)((AUDIO_OUT_RATE * ms) / 1000U);
    if (off + n > cap) n = cap - off;
    if (n <= 0) return off;
    if (freq == 0) {
        for (int i = 0; i < n; i++) buf[off + i] = 128;
        return off + n;
    }
    int half_period = (int)(AUDIO_OUT_RATE / (freq * 2U));
    if (half_period < 1) half_period = 1;
    int toggle = 0, cnt = 0;
    for (int i = 0; i < n; i++) {
        buf[off + i] = toggle ? 176 : 80; /* moderate square-wave amplitude around center 128 */
        if (++cnt >= half_period) { cnt = 0; toggle = !toggle; }
    }
    return off + n;
}

/* Plays up to two tones back-to-back as a single PCM buffer (avoids
 * needing to block on audio_busy() between them), or a two-beep
 * pcspkr fallback if no sound card was found. */
static void play2(uint32_t f1, uint32_t ms1, uint32_t f2, uint32_t ms2)
{
    if (!enabled) return;
    ensure_audio();
    if (audio_available()) {
        int off = fill_tone(snd_buf, 0, AUDIO_DMA_SIZE, f1, ms1);
        off = fill_tone(snd_buf, off, AUDIO_DMA_SIZE, f2, ms2);
        audio_submit(snd_buf, (uint32_t)off);
    } else {
        pcspkr_beep(f1, ms1);
        if (f2) pcspkr_beep(f2, ms2);
    }
}

void sound_set_enabled(int on) { enabled = on ? 1 : 0; }
int  sound_get_enabled(void)   { return enabled; }

void sound_click(void) { play2(1800, 15, 0, 0); }
void sound_open(void)  { play2(660, 30, 990, 40); }
void sound_close(void) { play2(880, 30, 550, 40); }
void sound_error(void) { play2(220, 60, 220, 60); }
void sound_notify(void) { play2(1320, 40, 1760, 70); }
