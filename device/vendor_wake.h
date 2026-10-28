#ifndef S12_VENDOR_WAKE_H
#define S12_VENDOR_WAKE_H
#include <stddef.h>
#include <stdint.h>

/* Exact S12A 1.76.54 libxaudio_engine.so adapter. No capture, playback,
 * networking, engine threads or vendor daemons are started here. The caller
 * must verify the original read-only system0 library identity before opening.
 * ABI reconstructed from its exported wrapper ARM instructions; target replay
 * is required before enabling this adapter in a released firmware. */
typedef struct s12_vendor_wake s12_vendor_wake;
typedef struct s12_vendor_frontend s12_vendor_frontend;
typedef struct s12_vendor_aec s12_vendor_aec;
s12_vendor_wake *s12_vendor_wake_open(char *error, size_t error_bytes);
/* PCM is 16 kHz, signed 16-bit little-endian mono, exactly 160 samples.
 * Return 1 on a new local keyword hit, 0 for no hit, -1 for adapter failure. */
int s12_vendor_wake_process(s12_vendor_wake *, const int16_t pcm[160]);
/* Optional metadata from the guarded original wrapper result. Frame indices
 * are relative to this detector's input stream: one frame = 160 samples at
 * 16 kHz (10 ms). input_frames counts completed process calls. A caller may
 * mix process/process_event without resetting that count. Range metadata is
 * usable only on a hit with timing_valid=1; it is zero on non-hit results.
 * These are model alignment ranges, not exact phonetic boundaries. Original
 * wake-only replay showed end270ms before the remaining spoken word tail;
 * callers must not classify all energy after end as a following command.
 * This API preserves the existing hit/no-hit/error return contract. */
struct s12_vendor_wake_event {
    uint64_t input_frames,begin_frame,end_frame;
    uint32_t status,timing_valid;
};
int s12_vendor_wake_process_event(s12_vendor_wake *,const int16_t pcm[160],struct s12_vendor_wake_event *);
const char *s12_vendor_wake_version(s12_vendor_wake *);
const char *s12_vendor_wake_error(s12_vendor_wake *);
void s12_vendor_wake_close(s12_vendor_wake *);
/* Original six-microphone AFE: 128 samples per channel, channel-planar, 16 kHz.
 * Return 1 with 160 beamformed samples, 0 while reframing, -1 on ABI failure.
 * The supplied wake handle owns the library and must outlive the frontend. */
s12_vendor_frontend *s12_vendor_frontend_open(s12_vendor_wake *, char *, size_t);
int s12_vendor_frontend_process(s12_vendor_frontend *, const int16_t [6][128], int16_t [160]);
const char *s12_vendor_frontend_error(s12_vendor_frontend *);
void s12_vendor_frontend_close(s12_vendor_frontend *);
/* Exact original six-channel AEC. Inputs are six planar microphone blocks and
 * one synchronous playback-reference block, all 16 kHz S16, exactly128 samples.
 * The wake handle owns the library and must outlive this handle. The caller
 * serializes all calls on one DSP owner; this API starts no threads or I/O.
 * process returns0 only after all6 original handlers returned256 output bytes
 * with guards and immutable input copies intact; out remains untouched on any
 * error. An ABI/process error poisons the handle until a successful reset.
 * No physical ALSA/reference-channel mapping or echo quality is assumed here.
 * reset destroys old states first to avoid doubling AEC memory on the256MiB
 * target, then creates6 fresh states; failure leaves a disabled empty handle.
 * close always frees the adapter and returns0, or -1 on destructor ABI/guard
 * failure. Passing NULL to close is safe. */
s12_vendor_aec *s12_vendor_aec_open(s12_vendor_wake *, char *, size_t);
int s12_vendor_aec_process(s12_vendor_aec *, const int16_t [6][128], const int16_t [128], int16_t [6][128]);
int s12_vendor_aec_reset(s12_vendor_aec *);
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
/* Private read-only config projection. No pointer is retained or exposed. */
struct s12_aec_debug_channel_params {
    int32_t denoise,noise_suppress,echo_suppress,echo_suppress_active;
    int32_t status[5];unsigned valid,linear_present,preprocess_present,connection_present,connection_matches_linear;
};
struct s12_aec_debug_params {
    unsigned epoch,valid_mask,error,error_channel;
    struct s12_aec_debug_channel_params channel[6];
};
#endif
const char *s12_vendor_aec_error(s12_vendor_aec *);
int s12_vendor_aec_close(s12_vendor_aec *);
#ifdef S12_VENDOR_WAKE_DIAGNOSTICS
struct s12_vendor_wake_diagnostics {
    uint32_t status[3];
    float l1_score, l1_threshold, l1_max_score;
    uint32_t l1_frames, l1_warmup, l1_flag;
    uint64_t result_begin_frame,result_end_frame;
    uint32_t impl_status,active;
    uint64_t previous_begin_frame,previous_end_frame;
};
void s12_vendor_wake_diagnostics(s12_vendor_wake *, struct s12_vendor_wake_diagnostics *);
#endif
#endif
