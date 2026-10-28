/* Minimal opaque ALSA PCM ABI declarations, checked against alsa-lib v1.1.4
 * include/pcm.h and include/global.h. No private structures are accessed.
 * Upstream reference copies and hashes are in source-manifest.json. */
#ifndef S12_ALSA_PCM_ABI_H
#define S12_ALSA_PCM_ABI_H
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
enum { S12_PLAYBACK = 0, S12_CAPTURE = 1, S12_NONBLOCK = 1,
       S12_RW_INTERLEAVED = 3, S12_S16_LE = 2, S12_S24_LE = 6,
       S12_S32_LE = 10, S12_STATE_SETUP = 1, S12_STATE_PREPARED = 2,
       S12_STATE_RUNNING = 3, S12_STATE_XRUN = 4,
       S12_STATE_DRAINING = 5, S12_STATE_SUSPENDED = 7,
       S12_STATE_DISCONNECTED = 8 };
extern const char *snd_asoundlib_version(void);
extern int snd_pcm_open(snd_pcm_t **, const char *, int, int);
extern int snd_pcm_close(snd_pcm_t *);
extern int snd_pcm_drop(snd_pcm_t *);
extern int snd_pcm_drain(snd_pcm_t *);
extern int snd_pcm_prepare(snd_pcm_t *);
extern int snd_pcm_start(snd_pcm_t *);
extern int snd_pcm_state(snd_pcm_t *);
extern int snd_pcm_wait(snd_pcm_t *, int);
extern snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *);
extern snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *, void *, snd_pcm_uframes_t);
extern snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
extern int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **);
extern void snd_pcm_hw_params_free(snd_pcm_hw_params_t *);
extern int snd_pcm_hw_params_any(snd_pcm_t *, snd_pcm_hw_params_t *);
extern int snd_pcm_hw_params_test_access(snd_pcm_t *, snd_pcm_hw_params_t *, int);
extern int snd_pcm_hw_params_test_format(snd_pcm_t *, snd_pcm_hw_params_t *, int);
extern int snd_pcm_hw_params_test_channels(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
extern int snd_pcm_hw_params_test_rate(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int);
extern int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *, unsigned *);
extern int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *, unsigned *);
extern int snd_pcm_hw_params_get_rate_min(const snd_pcm_hw_params_t *, unsigned *, int *);
extern int snd_pcm_hw_params_get_rate_max(const snd_pcm_hw_params_t *, unsigned *, int *);
extern int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *, int *);
extern int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *, unsigned *);
extern int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *, unsigned *, int *);
extern int snd_pcm_hw_params_set_access(snd_pcm_t *, snd_pcm_hw_params_t *, int);
extern int snd_pcm_hw_params_set_format(snd_pcm_t *, snd_pcm_hw_params_t *, int);
extern int snd_pcm_hw_params_set_channels(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
extern int snd_pcm_hw_params_set_rate(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int);
extern int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *);
extern int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
extern int snd_pcm_hw_params_get_buffer_size_max(const snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
extern int snd_pcm_get_params(snd_pcm_t *, snd_pcm_uframes_t *, snd_pcm_uframes_t *);
extern int snd_pcm_hw_params(snd_pcm_t *, snd_pcm_hw_params_t *);
extern int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **);
extern void snd_pcm_sw_params_free(snd_pcm_sw_params_t *);
extern int snd_pcm_sw_params_current(snd_pcm_t *, snd_pcm_sw_params_t *);
extern int snd_pcm_sw_params_set_avail_min(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
extern int snd_pcm_sw_params(snd_pcm_t *, snd_pcm_sw_params_t *);
#endif
