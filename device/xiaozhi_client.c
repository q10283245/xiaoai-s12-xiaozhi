/* S12 original-board minimal Xiaozhi WebSocket v1 client.
 * Independent implementation using the public protocol, not vendor services.
 * One-shot/remote-ASR compatibility and opt-in original S12 local KWS/BF.
 * Default half duplex; verified opt-in local AEC barge-in, no flash access.
 * ws:// is deliberately restricted to private/loopback IPv4 destinations.
 * Public references and build/run boundaries: XIAOZHI_CLIENT.md.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "xiaozhi_json_abi.h"
#ifndef XIAOZHI_TEST_NO_ALSA
#include "alsa_pcm_abi.h"
extern int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
#endif

typedef struct OpusDecoder OpusDecoder;
typedef struct OpusEncoder OpusEncoder;
extern OpusDecoder *opus_decoder_create(int, int, int *);
extern void opus_decoder_destroy(OpusDecoder *);
extern int opus_decode(OpusDecoder *, const unsigned char *, int, int16_t *, int, int);
extern OpusEncoder *opus_encoder_create(int,int,int,int *);
extern void opus_encoder_destroy(OpusEncoder *);
extern int opus_encode(OpusEncoder *,const int16_t *,int,unsigned char *,int);
extern int opus_encoder_ctl(OpusEncoder *,int,...);

#define MAX_JSON 65536u
#define MAX_HTTP 8192u
#define MAX_TOKEN 4096u
#define MAX_OPUS 4096u
#define DECODE_FRAMES 5760
#define MAX_AUDIO_PACKETS 10000u
#define MAX_MUSIC_AUDIO_PACKETS 12016u
#define MUSIC_IDLE_TIMEOUT_MS 15000
#define MUSIC_HARD_TIMEOUT_MS (13*60*1000)
#define TTS_BUFFER_BYTES (512u*1024u)
#define TTS_BUFFER_PACKETS 2000u
#define CAPTURE_CHANNELS 8u
#define CAPTURE_CHUNK 2880u
#define UPLINK_SAMPLES 960u
#define FILTER_TAPS 63u
#define WAKE_PREROLL_PACKETS 4u
#define WAKE_SPEECH_PACKETS 3u
#define WAKE_SILENCE_PACKETS 17u
#define COMMAND_SPEECH_SILENCE_PACKETS 20u
#define WAKE_MAX_PACKETS 200u
#define PI 3.14159265358979323846
#include "mic_frontend.h"
#include "vendor_wake.h"
#include "third_party/libfvad/amalgamation.h"
#include "followup_ready_pcm.h"
#include "ui_feedback_pcm.h"
#include "farewell_prompt_pcm.h"
_Static_assert(S12_FAREWELL_PROMPT_COUNT>=1&&S12_FAREWELL_PROMPT_COUNT<=8,"bounded farewell clips");
_Static_assert(S12_FAREWELL_PROMPT_RATE==16000,"farewell sample rate");

struct config { char url[2048], device[128], client[128], token_path[4096]; };
struct endpoint { char host[256], port[6], path[2048]; };
struct mcp_cached_volume {char id[160];int volume,result;const char *reason;};
enum dialogue_state { DIALOGUE_FALLBACK, DIALOGUE_AWAIT_REPLY, DIALOGUE_ALLOW_FOLLOWUP, DIALOGUE_CLOSE, DIALOGUE_LISTEN_ONLY };
enum command_prompt { COMMAND_PROMPT_NONE, COMMAND_PROMPT_FAREWELL, COMMAND_PROMPT_KEY };
struct wire { uint64_t mcp_epoch;unsigned mcp_initialized,mcp_cache_next;struct mcp_cached_volume mcp_cache[8];int fd; uint8_t pending[8192]; size_t pos, used; int64_t deadline;unsigned writing,incomplete,receiving,mcp_deferred;char turn_id[33];unsigned dialogue_seen,dialogue_invalid;enum dialogue_state dialogue_state;enum command_prompt command_prompt;unsigned command_feedback_capable,quiet_followup_capable,followup_request,quiet_completed,capture_diagnostics_capable,response_audio; };
struct message { unsigned opcode; size_t len; uint8_t data[MAX_JSON + 1]; };
struct audio {
    OpusDecoder *decoder;
    unsigned packets,played_packets;
    uint64_t samples;
    int playback, gain,buffer_tts,music,ui_prompt,ready_prompt;
#ifndef XIAOZHI_TEST_NO_ALSA
    snd_pcm_t *pcm;
#endif
};

/* Errors contain fixed stage names only: never echo server data, URL, token,
 * HTTP headers, config contents or text supplied by the caller. */
static volatile sig_atomic_t stopping;
/* Optional local panel IPC. All control files live on the private RAM root;
 * the panel daemon is the only writer. No server message can name a path. */
struct panel_control { uint32_t seq,gain,muted,cancel,manual; };
static struct panel_control panel={0,50,0,0,0};
static int panel_enabled,panel_active,panel_interrupted;
static int local_wake_enabled;
static int local_capture_recovering;
/* Main-thread-only local command feedback may be preempted without reopening capture. */
static int command_feedback_active;
static int ota_idle_requested(void);
static const char *barge_failure="none",*barge_error_reason="none";
static unsigned followup_seconds;
static void device_mcp_service(void);
static void device_mcp_cancel(struct wire *);
static void device_mcp_poll_input(struct wire *);
static int device_mcp_receive(struct wire *,struct json_object *);
static int64_t followup_capture_deadline,followup_onset_deadline;
static int followup_prompt_pending,followup_quiet_pending;
static struct {
    struct wire *wire;
    uint8_t payload[1024],frame[1040];size_t payload_bytes,bytes,sent;
    int64_t next,deadline;int failed;
} session_keepalive;
static void session_keepalive_poll(void);
static int session_keepalive_before_frame(struct wire *);
static void session_keepalive_unbind(void);
static void session_wire_close(struct wire *,int);
static int barge_in_worker(void);
static void barge_note_failure(const char *stage);
#ifndef XIAOZHI_TEST_NO_ALSA
static void barge_playback_settings(unsigned buffer,unsigned period);
#endif
/* Full duplex is a deliberate capability gate, never inferred from spare
 * ALSA channels. PID 1 supplies the result of a device-specific reference
 * validation; ordinary installations retain the original half-duplex path. */
static struct { int enabled;unsigned reference,gain; } barge_config;
/* Production always uses the configured profile. The RAM-only debug shim
 * permits testing reference7 while an older, pinned profile6 is installed. */
static unsigned barge_effective_reference(void) {
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_G4R7_TRIAL_REFERENCE) && S12_DEBUG_G4R7_TRIAL_REFERENCE
    return 7;
#else
    return barge_config.reference;
#endif
}
static int barge_active;
static void barge_service(void);
static struct {
    int enabled,pending,ready_after_capture,ready_visible;
    int64_t ready_until;
} startup_feedback;
/* Main audio owner only: control reads enqueue a short cue; they never open
 * ALSA or touch capture/DSP from a second thread. */
static struct {
    unsigned initialized,pending,active,offset;
    int64_t first,updated,last;
} ui_feedback;
static int ui_feedback_ready(void);
static void ui_feedback_changed(const struct panel_control *,const struct panel_control *);
#ifndef XIAOZHI_TEST_NO_ALSA
static void ui_feedback_overlay(int32_t *,int,int);
#endif
static uint32_t panel_manual_used;
static int64_t panel_next_poll;
static void panel_poll(void);
static void panel_state(const char *state);
static void local_wake_state(const char *state);
static int panel_parse(const char *text,struct panel_control *out);
static void request_stop(int signo) { (void)signo;stopping=1; }
static int fail(const char *stage) { barge_note_failure(stage);fprintf(stderr, "xiaozhi: %s\n", stage);return -1; }
static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int remaining(int64_t deadline) {
    int64_t n = now_ms(), r = deadline - n;
    return n < 0 || r <= 0 ? 0 : r > INT_MAX ? INT_MAX : (int)r;
}
static int ready_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        panel_poll();if(panel_interrupted||session_keepalive.failed)return -1;
        if(stopping)return fail("interrupted");
        int ms = remaining(deadline);
        if (!ms) return fail("deadline expired");
        struct pollfd p = { fd, events, 0 };
        int limit=barge_active||session_keepalive.wire?20:panel_enabled?100:ms;
        int slice=ms>limit?limit:ms;
        int r = poll(&p, 1, slice);
        if (r < 0 && errno == EINTR) {if(stopping)return fail("interrupted");continue;}
        if(r==0&&slice<ms)continue;
        if (r <= 0) return fail(r == 0 ? "deadline expired" : "socket poll failed");
        if (p.revents & events) return 0;
        return fail("socket closed or failed");
    }
}
static int random_bytes(uint8_t *out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("random source unavailable");
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, out + off, n - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return fail("random source failed"); }
        off += (size_t)r;
    }
    close(fd); return 0;
}
static int socket_write(struct wire *w, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        if (ready_fd(w->fd, POLLOUT, w->deadline)) return -1;
        ssize_t r = send(w->fd, p, n, 0);
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (r <= 0) return fail("socket write failed");
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int socket_read(struct wire *w, void *data, size_t n) {
    uint8_t *p = data;
    while (n) {
        if (w->pos == w->used) {
            if (ready_fd(w->fd, POLLIN, w->deadline)) return -1;
            ssize_t r = recv(w->fd, w->pending, sizeof(w->pending), 0);
            if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (!r)return fail("socket peer closed");
            if (r < 0) return fail("socket read failed");
            w->pos = 0; w->used = (size_t)r;
        }
        size_t k = w->used - w->pos;
        if (k > n) k = n;
        memcpy(p, w->pending + w->pos, k);
        w->pos += k; p += k; n -= k;
    }
    return 0;
}
static void wipe(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }

/* Ignore the initial snapshot and duplicate sequence/RPC responses. Sliding
 * changes settle for 160 ms, with a 400 ms maximum delay and 200 ms rate cap. */
static void ui_feedback_changed(const struct panel_control *old,const struct panel_control *next) {
    if(!ui_feedback.initialized){ui_feedback.initialized=1;return;}
    unsigned kind=(next->muted!=old->muted||next->cancel!=old->cancel)?2:next->gain!=old->gain?1:0;
    if(!next->gain){ui_feedback.pending=ui_feedback.active=0;return;}
    /* Manual start plays its key tone at the existing wake-prompt boundary. */
    if(next->manual!=old->manual){ui_feedback.pending=0;return;}
    if(!kind)return;
    int64_t n=now_ms();if(n<0)return;
    if(!ui_feedback.pending)ui_feedback.first=n;
    if(kind>ui_feedback.pending)ui_feedback.pending=kind;
    ui_feedback.updated=n;
}
static int ui_feedback_ready(void) {
    int64_t n=now_ms();return ui_feedback.pending&&n>=0&&
        (!ui_feedback.last||n-ui_feedback.last>=200)&&
        (n-ui_feedback.updated>=160||n-ui_feedback.first>=400);
}
#ifndef XIAOZHI_TEST_NO_ALSA
static void ui_feedback_overlay(int32_t *stereo,int frames,int gain) {
    if(!gain){ui_feedback.pending=ui_feedback.active=0;return;}
    if(!ui_feedback.active&&ui_feedback_ready()) {
        ui_feedback.active=ui_feedback.pending;ui_feedback.pending=0;ui_feedback.offset=0;ui_feedback.last=now_ms();
        puts("{\"event\":\"ui_feedback\",\"mode\":\"mixed\",\"microphone_uploaded\":false}");
    }
    if(!ui_feedback.active)return;
    const uint8_t *pcm=ui_feedback.active==1?s12_volume_prompt_pcm:s12_button_prompt_pcm;
    unsigned bytes=ui_feedback.active==1?S12_VOLUME_PROMPT_PCM_BYTES:S12_BUTTON_PROMPT_PCM_BYTES;
    unsigned end=bytes/2*3;
    for(int i=0;i<frames&&ui_feedback.offset<end;++i,++ui_feedback.offset) {
        unsigned at=ui_feedback.offset/3*2;
        int16_t sample=(int16_t)((unsigned)pcm[at]|(unsigned)pcm[at+1]<<8);
        int64_t add=(int64_t)sample*gain*65536/1000;
        for(unsigned ch=0;ch<2;++ch) {
            int64_t value=(int64_t)stereo[i*2+ch]+add;
            stereo[i*2+ch]=value>INT32_MAX?INT32_MAX:value<INT32_MIN?INT32_MIN:(int32_t)value;
        }
    }
    if(ui_feedback.offset>=end)ui_feedback.active=0;
}
#endif
/* glibc 2.25 ARM expects old xstat ABI version 3. Zig 0.13's generic
 * fstat-2.32 compatibility wrapper passes version 0, which this old libc
 * rejects. Use its exported 64-bit-inode ABI explicitly, with compile-time
 * layout checks against the official old ARM struct. Native tests use POSIX.
 * https://github.com/bminor/glibc/blob/glibc-2.25/sysdeps/unix/sysv/linux/bits/stat.h
 */
#if defined(__linux__) && defined(__arm__) && defined(__GLIBC__)
extern int __fxstat64(int,int,void *);
_Static_assert(sizeof(struct stat)==104,"ARM glibc stat64 size");
_Static_assert(offsetof(struct stat,st_mode)==16,"ARM stat64 mode offset");
_Static_assert(offsetof(struct stat,st_uid)==24,"ARM stat64 uid offset");
_Static_assert(offsetof(struct stat,st_size)==48,"ARM stat64 size offset");
_Static_assert(offsetof(struct stat,st_ino)==96,"ARM stat64 inode offset");
#endif
static int metadata_stat(int fd,struct stat *s) {
#if defined(__linux__) && defined(__arm__) && defined(__GLIBC__)
    return __fxstat64(3,fd,s);
#else
    return fstat(fd,s);
#endif
}
#include "s12_debug_diag.h"
static int file_abi_check(const char *path) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){printf("{\"event\":\"file_abi_check\",\"open_rc\":-1,\"errno\":%d}\n",errno);return 1;}
    struct stat s;memset(&s,0,sizeof(s));errno=0;
    int rc=metadata_stat(fd,&s),error=errno;
    close(fd);
    printf("{\"event\":\"file_abi_check\",\"rc\":%d,\"errno\":%d,\"struct_bytes\":%u",rc,error,(unsigned)sizeof(s));
    if(!rc)printf(",\"mode\":%u,\"uid\":%u,\"gid\":%u,\"size\":%lld,\"ino\":%llu,\"regular\":%s",(unsigned)s.st_mode,
        (unsigned)s.st_uid,(unsigned)s.st_gid,(long long)s.st_size,(unsigned long long)s.st_ino,S_ISREG(s.st_mode)?"true":"false");
    puts("}");return rc?1:0;
}

static int read_file(const char *path, char *buf, size_t cap, int secret) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fail(secret ? "token file open failed" : "config open failed");
    struct stat st;
    if(metadata_stat(fd,&st)) {int error=errno;close(fd);fprintf(stderr,"xiaozhi: file metadata query failed (errno=%d)\n",error);return -1;}
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size >= cap ||
        (secret && ((st.st_mode & 077) != 0 || st.st_uid != geteuid()))) {
        close(fd); return fail(secret ? "token file must be owned private regular file" : "invalid config file");
    }
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t r = read(fd, buf + total, cap - 1 - total);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { close(fd); return fail("file read failed"); }
        if (!r) break;
        total += (size_t)r;
    }
    close(fd);
    if (total != (size_t)st.st_size || memchr(buf, 0, total)) return fail("invalid file contents");
    buf[total] = 0; return (int)total;
}
static int panel_parse(const char *text,struct panel_control *out) {
    if(strncmp(text,"v1 ",3))return -1;
    const char *p=text+3;uint32_t v[5];
    for(unsigned i=0;i<5;++i) {
        if(*p<'0'||*p>'9')return -1;
        uint64_t n=0;
        do {n=n*10+(unsigned)(*p++-'0');if(n>UINT32_MAX)return -1;}while(*p>='0'&&*p<='9');
        v[i]=(uint32_t)n;
        if(i<4){if(*p++!=' ')return -1;}
    }
    if(*p=='\n')++p;
    if(*p||v[1]>200||v[2]>1)return -1;
    *out=(struct panel_control){v[0],v[1],v[2],v[3],v[4]};return 0;
}
static int panel_path(char *out,size_t cap,const char *name) {
    const char *dir="/run/s12";
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *test=getenv("S12_TEST_PANEL_DIR");if(test&&*test)dir=test;
#endif
    int n=snprintf(out,cap,"%s/%s",dir,name);return n>0&&(size_t)n<cap?0:-1;
}
static void panel_poll_controls(void) {
    if(!panel_enabled)return;
    int64_t n=now_ms();if(n>=0&&n<panel_next_poll)return;
    panel_next_poll=n+40;
    char path[4096],raw[160];if(panel_path(path,sizeof(path),"panel.control"))return;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return;
    struct stat st;int good=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&
        !(st.st_mode&077)&&st.st_size>0&&st.st_size<(off_t)sizeof(raw);
    ssize_t count=good?read(fd,raw,(size_t)st.st_size):-1;close(fd);
    if(!good||count!=st.st_size||memchr(raw,0,(size_t)count))return;
    raw[count]=0;struct panel_control next;
    if(panel_parse(raw,&next)||next.seq<panel.seq||next.cancel<panel.cancel||next.manual<panel.manual)return;
    if(panel_active&&(next.cancel!=panel.cancel||next.muted!=panel.muted))panel_interrupted=1;
    ui_feedback_changed(&panel,&next);panel=next;
}
/* Main-thread controls precede the short worker-mailbox pump. The capture
 * reader only enqueues samples; one separate worker owns every vendor DSP
 * context until a joined handoff. Mute wins before main authorizes a hit. */
static void panel_poll(void) {
    panel_poll_controls();s12dbg_main(panel.muted,panel.gain);
    if(command_feedback_active&&((panel_enabled&&(panel.muted||panel.manual!=panel_manual_used))||ota_idle_requested()))panel_interrupted=1;
    barge_service();session_keepalive_poll();device_mcp_service();
}
static void panel_state(const char *state) {
    if(!panel_enabled)return;
    char path[4096],tmp[4096];
    if(panel_path(path,sizeof(path),"ui.state")||panel_path(tmp,sizeof(tmp),"ui.state.client-new"))return;
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return;
    char line[32];int len=snprintf(line,sizeof(line),"%s\n",panel.muted?"muted":state);
    int ok=len>0&&(size_t)len<sizeof(line)&&write(fd,line,(size_t)len)==len;
    if(close(fd))ok=0;
    if(ok){if(rename(tmp,path))unlink(tmp);}else unlink(tmp);
}
static void local_wake_state(const char *state) {
    if(!local_wake_enabled)return;s12dbg_phase(state);
    char path[4096],tmp[4096];
    if(panel_path(path,sizeof(path),"local-wake.state")||panel_path(tmp,sizeof(tmp),"local-wake.state.client-new"))return;
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return;
    char line[32];int n=snprintf(line,sizeof(line),"%s\n",state);
    int ok=n>0&&(size_t)n<sizeof(line)&&write(fd,line,(size_t)n)==n;
    if(close(fd))ok=0;
    if(ok){if(rename(tmp,path))unlink(tmp);}else unlink(tmp);
}
/* Fixed package/RAM directories only. Native fixture paths are compiled out
 * of production. Directory FDs anchor no-follow, single-link private files. */
static int startup_directory(int config) {
    const char *path=config?"/s12":"/run/s12";
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *test=getenv(config?"S12_TEST_STARTUP_CONFIG_DIR":"S12_TEST_PANEL_DIR");
    if(test&&*test)path=test;
#endif
    size_t len=strlen(path);if(!len||path[len-1]=='/')return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return -1;
    struct stat st;
    if(metadata_stat(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0777)!=0700){close(fd);return -1;}
    return fd;
}
static int startup_file(int dir,const char *name,const char *expected,size_t bytes) {
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;char value[16];
    int good=bytes<sizeof(value)&&!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&
        st.st_nlink==1&&(st.st_mode&0777)==0600&&st.st_size==(off_t)bytes;
    ssize_t got=good?read(fd,value,bytes+1):-1;
    int closed=close(fd);
    return good&&got==(ssize_t)bytes&&!closed&&!memcmp(value,expected,bytes)?1:-1;
}
static void startup_setup(void) {
    memset(&startup_feedback,0,sizeof(startup_feedback));
    int dir=startup_directory(1);if(dir<0)return;
    int enabled=startup_file(dir,"startup-feedback-enabled","yes\n",4);close(dir);
    if(enabled<0){puts("{\"event\":\"startup_feedback_disabled\",\"reason\":\"invalid_opt_in\"}");return;}
    if(enabled){startup_feedback.enabled=startup_feedback.pending=1;local_capture_recovering=1;panel_state("boot");}
}
/* Claim before playback, including failed/cancelled attempts. A client restart
 * or capture recovery cannot repeat the cue during this RAM-root boot epoch. */
static int startup_claim(void) {
    int dir=startup_directory(0);if(dir<0)return -1;
    static const char text[]="claimed\n";
    int fd=openat(dir,"startup-feedback.claimed",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if(fd<0){int error=errno;int existing=error==EEXIST?startup_file(dir,"startup-feedback.claimed",text,sizeof(text)-1):-1;close(dir);return existing==1?0:-1;}
    struct stat st;int good=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&
        st.st_nlink==1&&(st.st_mode&0777)==0600&&st.st_size==0;
    if(good)good=write(fd,text,sizeof(text)-1)==(ssize_t)sizeof(text)-1;
    if(close(fd))good=0;close(dir);
    return good?1:-1;
}
static void startup_ready_clear(void) {
    startup_feedback.ready_until=0;startup_feedback.ready_visible=0;
}
static void startup_idle_ui(void) {
    if(!startup_feedback.enabled)return;
    int64_t now=now_ms();
    if(startup_feedback.ready_after_capture) {
        startup_feedback.ready_after_capture=0;
        if(now>=0){startup_feedback.ready_until=now+2000;startup_feedback.ready_visible=1;panel_state("ready");}
    }
    if(startup_feedback.ready_visible&&(now<0||now>=startup_feedback.ready_until)) {
        startup_ready_clear();panel_state("idle");
    }
}
/* PID 1 pins the exact original library hash before issuing this root-only
 * marker. Independently require a read-only original system0 mount, so a
 * replacement file in the writable RAM root cannot acquire that identity. */
static int vendor_identity_verified(void) {
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    return 0; /* Native tests link a separate deterministic no-SDK fixture. */
#else
    static const char expected[]="79f1a33d9683cd6940c8d23e3dd4e992f1958fe220c0dfff8d230f9f8a1b5e73\n";
    int fd=open("/run/s12/vendor-wake.verified",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return fail("original wake library verification marker missing");
    struct stat st;char hash[sizeof(expected)];
    int good=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_nlink==1&&
        (st.st_mode&0777)==0600&&st.st_size==65;
    ssize_t n=good?read(fd,hash,sizeof(hash)):-1;close(fd);
    if(!good||n!=65||memcmp(hash,expected,65))return fail("original wake library identity not verified");
    fd=open("/proc/self/mounts",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return fail("original system mount check unavailable");
    char mounts[32768];size_t at=0;
    while(at<sizeof(mounts)-1){ssize_t r=read(fd,mounts+at,sizeof(mounts)-1-at);if(r<0&&errno==EINTR)continue;if(r<0){close(fd);return -1;}if(!r)break;at+=(size_t)r;}
    close(fd);if(at==sizeof(mounts)-1)return fail("original system mount table too large");mounts[at]=0;
    char *line=mounts;
    while(*line){char *end=strchr(line,'\n');if(end)*end=0;
        char device[256],point[256],type[64],opts[512];
        if(sscanf(line,"%255s %255s %63s %511s",device,point,type,opts)==4&&
           !strcmp(point,"/run/s12/system0")&&!strcmp(type,"squashfs")) {
            int ro=0;char *p=opts;
            while(*p){char *e=strchr(p,',');size_t bytes=e?(size_t)(e-p):strlen(p);if(bytes==2&&!memcmp(p,"ro",2))ro=1;if(!e)break;p=e+1;}
            if(ro)return 0;
        }
        if(!end)break;line=end+1;
    }
    return fail("original system0 must be mounted read-only squashfs");
#endif
}
/* Automatic firmware apply uses a RAM-only producer/consumer handshake.
 * Merely seeing ui.state=listening does not authorize flashing: this client
 * closes its sole microphone handle before acknowledging a private nonce.
 * Files remain fixed; no network message can supply an IPC path. */
static int ota_idle_directory(void) {
    const char *path="/run/s12/ota";
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *test=getenv("S12_TEST_OTA_DIR");if(test&&*test)path=test;
#endif
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    struct stat st;
    if(fd<0)return -1;
    if(metadata_stat(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0777)!=0700){close(fd);return -1;}
    return fd;
}
static int ota_idle_nonce(int directory,const char *name,char nonce[50]) {
    int fd=openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return 0;
    struct stat st;char raw[50];
    int ok=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&
        (st.st_mode&0777)==0600&&st.st_size==49;
    ssize_t n=ok?read(fd,raw,sizeof(raw)):-1;close(fd);
    if(!ok||n!=49||raw[48]!='\n')return 0;
    for(unsigned i=0;i<48;++i)if(!((raw[i]>='0'&&raw[i]<='9')||(raw[i]>='a'&&raw[i]<='f')))return 0;
    memcpy(nonce,raw,49);nonce[49]=0;return 1;
}
static int ota_idle_requested(void) {
    int dir=ota_idle_directory();if(dir<0)return 0;
    char nonce[50];int present=ota_idle_nonce(dir,"auto-apply.request",nonce);close(dir);return present;
}
static int ota_idle_wait(void) {
    int dir=ota_idle_directory();if(dir<0)return 0;
    char nonce[50],current[50],temp[80];
    if(!ota_idle_nonce(dir,"auto-apply.request",nonce)){close(dir);return 0;}
    snprintf(temp,sizeof(temp),".auto-apply.idle.client-%ld",(long)getpid());
    int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){close(dir);return 0;}
    int ok=write(fd,nonce,49)==49;if(close(fd))ok=0;
    if(!ok||!ota_idle_nonce(dir,"auto-apply.request",current)||strcmp(current,nonce)||
       renameat(dir,temp,dir,"auto-apply.idle")) {
        unlinkat(dir,temp,0);close(dir);return 0;
    }
    panel_active=0;panel_interrupted=0;panel_state("idle");
    local_wake_state(local_capture_recovering?"recovering":"paused");
    puts("{\"event\":\"ota_idle_enter\",\"capture_closed\":true}");
    while(!stopping&&ota_idle_nonce(dir,"auto-apply.request",current)&&!strcmp(current,nonce)) {
        panel_poll();panel_state("idle");
        struct timespec pause={0,100000000};nanosleep(&pause,NULL);
    }
    /* Only remove our own acknowledgment. A newer generation remains owned
     * by its producer/consumer pair. Never follow an IPC symlink. */
    if(ota_idle_nonce(dir,"auto-apply.idle",current)&&!strcmp(current,nonce))unlinkat(dir,"auto-apply.idle",0);
    close(dir);puts("{\"event\":\"ota_idle_exit\"}");return 1;
}
static int panel_selftest(void) {
    struct panel_control parsed;
    if(panel_parse("v1 2 50 1 3 4\n",&parsed)||parsed.gain!=50||parsed.muted!=1||parsed.cancel!=3||parsed.manual!=4)return -1;
    const char *bad[]={"v1 -1 50 0 0 0\n","v1 0 201 0 0 0\n","v1 0 202 0 0 0\n","v1 0 50 2 0 0\n",
        "v1 4294967296 50 0 0 0\n","v1 0 50 0 0 0\nextra","v1 0 50 0 0 0 0\n","v2 0 50 0 0 0\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)if(!panel_parse(bad[i],&parsed))return -1;
    return 0;
}
static int utf8_valid(const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;) {
        uint32_t c=p[i++], min=0;unsigned extra=0;
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){c&=31;min=0x80;extra=1;}
        else if(c>=0xe0&&c<=0xef){c&=15;min=0x800;extra=2;}
        else if(c>=0xf0&&c<=0xf4){c&=7;min=0x10000;extra=3;}
        else return 0;
        if(extra>n-i)return 0;
        while(extra--){uint8_t q=p[i++];if((q&0xc0)!=0x80)return 0;c=(c<<6)|(q&63);}
        if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;
    }
    return 1;
}
/* json-c 0.12 exposes no opaque parse-end accessor. Check object framing
 * independently, then use its strict grammar parser; never rely on a private
 * struct layout or silently accept a second JSON value/trailing garbage. */
static int one_json_object(const uint8_t *p,size_t n) {
    size_t i=0,depth=0;uint8_t stack[32];int quoted=0,escaped=0,ended=0;
    while(i<n&&(p[i]==' '||p[i]=='\t'||p[i]=='\r'||p[i]=='\n'))++i;
    if(i==n||p[i]!='{')return 0;
    for(;i<n;++i) {
        uint8_t c=p[i];
        if(ended){if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n')return 0;continue;}
        if(quoted){if(c<32)return 0;if(escaped)escaped=0;else if(c=='\\')escaped=1;else if(c=='"')quoted=0;continue;}
        if(c=='"'){quoted=1;continue;}
        if(c=='{'||c=='['){if(depth==sizeof(stack))return 0;stack[depth++]=c;}
        else if(c=='}'||c==']') {
            if(!depth||stack[--depth]!=(c=='}'?'{':'['))return 0;
            if(!depth)ended=1;
        }
    }
    return ended&&!quoted;
}
static struct json_object *parse_json(const void *data, size_t n) {
    if (n == 0 || n > MAX_JSON || memchr(data, 0, n)) return NULL;
    if (!utf8_valid(data,n)||!one_json_object(data,n))return NULL;
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, data, n); copy[n] = 0;
    struct json_tokener *t = json_tokener_new();
    if (!t) { free(copy); return NULL; }
    json_tokener_set_flags(t,1); /* JSON_TOKENER_STRICT, public since 0.11 */
    struct json_object *j = json_tokener_parse_ex(t, copy, (int)n + 1);
    if (json_tokener_get_error(t) != 0) { if (j) json_object_put(j); j = NULL; }
    json_tokener_free(t); free(copy);
    if (j && !json_object_is_type(j, json_type_object)) { json_object_put(j); j = NULL; }
    return j;
}
static struct json_object *field(struct json_object *o, const char *key, enum json_type type) {
    struct json_object *v = NULL;
    return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, type) ? v : NULL;
}
static const char *string_field(struct json_object *o, const char *key) {
    struct json_object *v = field(o, key, json_type_string);
    if (!v) return NULL;
    const char *s = json_object_get_string(v);
    int len = json_object_get_string_len(v);
    return s && len >= 0 && strlen(s) == (size_t)len ? s : NULL;
}
static int copy_field(struct json_object *o, const char *key, char *out, size_t cap) {
    const char *s = string_field(o, key);
    if (!s || !*s || strlen(s) >= cap) return fail("missing or invalid config field");
    strcpy(out, s); return 0;
}
static int header_value(const char *s) {
    if (!*s) return 0;
    for (; *s; ++s) if ((unsigned char)*s < 33 || (unsigned char)*s > 126) return 0;
    return 1;
}
static int load_config_fields(const char *path, struct config *cfg) {
    char buf[8192];
    int n = read_file(path, buf, sizeof(buf), 0);
    if (n < 0) return -1;
    struct json_object *j = parse_json(buf, (size_t)n);
    if (!j) return fail("config JSON invalid");
    int r = copy_field(j, "url", cfg->url, sizeof(cfg->url)) ||
        copy_field(j, "deviceid", cfg->device, sizeof(cfg->device)) ||
        copy_field(j, "clientid", cfg->client, sizeof(cfg->client)) ||
        copy_field(j, "token-file", cfg->token_path, sizeof(cfg->token_path));
    json_object_put(j);
    if (r || !header_value(cfg->device) || !header_value(cfg->client)) return fail("config header value invalid");
    return 0;
}
static int barge_integer(struct json_object *j,const char *key,int low,int high,int *out) {
    struct json_object *v=field(j,key,json_type_int);if(!v)return -1;
    int n=json_object_get_int(v);if(n<low||n>high)return -1;*out=n;return 0;
}
static int barge_read_validation(const char *path,char raw[2048]) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return fail("barge reference validation file missing");
    struct stat st;unsigned owner=0;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    owner=(unsigned)geteuid();
#endif
    int good=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==owner&&st.st_nlink==1&&
        (st.st_mode&0777)==0600&&st.st_size>0&&st.st_size<2048;
    size_t used=0;
    if(good)while(used<(size_t)st.st_size) {
        ssize_t n=read(fd,raw+used,(size_t)st.st_size-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){good=0;break;}used+=(size_t)n;
    }
    char extra;ssize_t more=good?read(fd,&extra,1):-1;close(fd);
    if(!good||more!=0)return fail("barge validation requires root private single-link regular file");
    raw[used]=0;return (int)used;
}
static int barge_load_config(const char *path) {
    char raw[2048],marker[2048];int n=barge_read_validation(path,raw);if(n<0)return -1;
    const char *marker_path="/run/s12/barge-reference.verified";
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *test_marker=getenv("S12_TEST_BARGE_MARKER");if(test_marker&&*test_marker)marker_path=test_marker;
#endif
    int marker_bytes=barge_read_validation(marker_path,marker);
    int matches=marker_bytes==n&&!memcmp(raw,marker,(size_t)n);wipe(marker,sizeof(marker));
    if(!matches){wipe(raw,sizeof(raw));return fail("barge reference config does not match verified boot marker");}
    struct json_object *j=parse_json(raw,(size_t)n);if(!j)return fail("barge reference config JSON invalid");
    int schema=0,rate=0,channels=0,reference=0,delay=0,gain=0;
    struct json_object *verified=field(j,"reference_verified",json_type_boolean);
    const char *sha=string_field(j,"vendor_sha256"),*device=string_field(j,"device"),*image=string_field(j,"source_image");
    int valid=verified&&json_object_get_int(verified)==1&&
        !barge_integer(j,"schema",1,1,&schema)&&!barge_integer(j,"capture_rate",48000,48000,&rate)&&
        !barge_integer(j,"capture_channels",8,8,&channels)&&!barge_integer(j,"ref_channel",6,7,&reference)&&
        !barge_integer(j,"ref_delay_samples",0,0,&delay)&&!barge_integer(j,"ref_gain_permille",1,4000,&gain)&&
        sha&&!strcmp(sha,"79f1a33d9683cd6940c8d23e3dd4e992f1958fe220c0dfff8d230f9f8a1b5e73")&&
        device&&!strcmp(device,"S12A")&&image&&strlen(image)==64;
    if(valid)for(unsigned i=0;i<64;++i)if(!((image[i]>='0'&&image[i]<='9')||(image[i]>='a'&&image[i]<='f')))valid=0;
    json_object_put(j);wipe(raw,sizeof(raw));
    if(!valid)return fail("barge reference config not validated for this capture contract");
    barge_config.reference=(unsigned)reference;barge_config.gain=(unsigned)gain;return 0;
}
static int load_token(const struct config *cfg,char token[MAX_TOKEN+1]) {
    int n = read_file(cfg->token_path, token, MAX_TOKEN + 1, 1);
    if (n < 0) return -1;
    /* Accept one conventional final LF or CRLF, never embedded newlines. */
    if (n && token[n - 1] == '\n') token[--n] = 0;
    if (n && token[n - 1] == '\r') token[--n] = 0;
    if (n == 0) return fail("token file empty");
    for (int i = 0; i < n; ++i)
        if ((unsigned char)token[i] < 32 || (unsigned char)token[i] > 126)
            return fail("token file contains invalid header bytes");
    if (token[0] == ' ' || token[n - 1] == ' ') return fail("token file has surrounding spaces");
    return 0;
}
static int load_config(const char *path,struct config *cfg,char token[MAX_TOKEN+1]) {
    return load_config_fields(path,cfg)||load_token(cfg,token)?-1:0;
}
static int parse_url(const char *url, struct endpoint *e) {
    if (strncmp(url, "ws://", 5)) return fail("only private-LAN ws:// URLs supported");
    const char *a = url + 5, *slash = strchr(a, '/');
    const char *end = slash ? slash : a + strlen(a);
    const char *colon = memchr(a, ':', (size_t)(end - a));
    size_t hn = (size_t)((colon ? colon : end) - a);
    if (hn == 0 || hn >= sizeof(e->host)) return fail("invalid URL host");
    for (size_t i = 0; i < hn; ++i)
        if (!isalnum((unsigned char)a[i]) && a[i] != '-' && a[i] != '.') return fail("invalid URL host");
    memcpy(e->host, a, hn); e->host[hn] = 0;
    strcpy(e->port, "80");
    if (colon) {
        size_t pn = (size_t)(end - colon - 1);
        if (pn == 0 || pn >= sizeof(e->port)) return fail("invalid URL port");
        unsigned p = 0;
        for (size_t i = 0; i < pn; ++i) {
            unsigned char c = (unsigned char)colon[i + 1];
            if (!isdigit(c)) return fail("invalid URL port");
            p = p * 10 + c - '0';
        }
        if (!p || p > 65535) return fail("invalid URL port");
        memcpy(e->port, colon + 1, pn); e->port[pn] = 0;
    }
    const char *p = slash ? slash : "/";
    if (strlen(p) >= sizeof(e->path)) return fail("URL path too long");
    for (const char *q = p; *q; ++q)
        if ((unsigned char)*q < 33 || (unsigned char)*q > 126 || *q == '#') return fail("invalid URL path");
    strcpy(e->path, p); return 0;
}
static int private_ipv4(uint32_t net) {
    uint32_t x = ntohl(net);
    return (x >> 24) == 127 || (x >> 24) == 10 || (x >> 20) == 0xac1 ||
           (x >> 16) == 0xc0a8 || (x >> 16) == 0xa9fe;
}
static int connect_lan(struct wire *w, const struct endpoint *e) {
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    /* getaddrinfo may block in libc; the process watchdog also bounds DNS. */
    struct in_addr numeric;if(inet_pton(AF_INET,e->host,&numeric)==1)hints.ai_flags=AI_NUMERICHOST;
    if (getaddrinfo(e->host, e->port, &hints, &list)) return fail("host resolution failed");
    int result = -1;
    for (struct addrinfo *it = list; it; it = it->ai_next) {
        struct sockaddr_in *a = (struct sockaddr_in *)it->ai_addr;
        if (!private_ipv4(a->sin_addr.s_addr)) continue;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC)) { close(fd); continue; }
        int rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc && errno != EINPROGRESS) { close(fd); continue; }
        if (rc && ready_fd(fd, POLLOUT, w->deadline)) { close(fd); break; }
        int err = 0; socklen_t n = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) || err) { close(fd); continue; }
        w->fd = fd; result = 0; break;
    }
    freeaddrinfo(list);
    return result ? fail("private-LAN connection failed") : 0;
}

/* SHA-1 is used solely for RFC6455's public opening-handshake nonce. */
static uint32_t rol32(uint32_t x, unsigned n) { return x << n | x >> (32 - n); }
static void sha1(const uint8_t *data, size_t len, uint8_t hash[20]) {
    uint8_t b[128]; uint32_t h[5] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    size_t total = ((len + 9 + 63) / 64) * 64;
    /* Handshake input is exactly 60 bytes. Also used by the 3-byte selftest. */
    if (total > sizeof(b)) abort();
    memset(b, 0, total); memcpy(b, data, len); b[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) b[total - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)b[off+i*4]<<24 | (uint32_t)b[off+i*4+1]<<16 |
            (uint32_t)b[off+i*4+2]<<8 | b[off+i*4+3];
        for (int i = 16; i < 80; ++i) w[i] = rol32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h[0], c=h[2], d=h[3], e=h[4], v=h[1];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f=(v&c)|(~v&d); k=0x5a827999; }
            else if (i < 40) { f=v^c^d; k=0x6ed9eba1; }
            else if (i < 60) { f=(v&c)|(v&d)|(c&d); k=0x8f1bbcdc; }
            else { f=v^c^d; k=0xca62c1d6; }
            uint32_t t=rol32(a,5)+f+e+k+w[i]; e=d; d=c; c=rol32(v,30); v=a; a=t;
        }
        h[0]+=a; h[1]+=v; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i=0;i<20;++i) hash[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
}
static void base64(const uint8_t *in, size_t n, char *out) {
    static const char chars[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j=0;
    for (size_t i=0;i<n;i+=3) {
        uint32_t v=(uint32_t)in[i]<<16;
        if (i+1<n) v|=(uint32_t)in[i+1]<<8;
        if (i+2<n) v|=in[i+2];
        out[j++]=chars[v>>18]; out[j++]=chars[(v>>12)&63];
        out[j++]=i+1<n?chars[(v>>6)&63]:'='; out[j++]=i+2<n?chars[v&63]:'=';
    }
    out[j]=0;
}
static int token_contains(const char *s, const char *want) {
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') ++s;
        const char *e=s; while (*e && *e!=',') ++e;
        const char *trim=e; while (trim>s && (trim[-1]==' ' || trim[-1]=='\t')) --trim;
        if ((size_t)(trim-s)==strlen(want) && !strncasecmp(s,want,(size_t)(trim-s))) return 1;
        s=e;
    }
    return 0;
}
static int handshake(struct wire *w, const struct endpoint *e, const struct config *c, const char *token) {
    uint8_t rand[16], digest[20]; char key[25], expect[29], input[61];
    if (random_bytes(rand, sizeof(rand))) return -1;
    base64(rand,sizeof(rand),key);
    snprintf(input,sizeof(input),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    sha1((const uint8_t *)input,strlen(input),digest); base64(digest,sizeof(digest),expect);
    char request[8192];
    const char *prefix=strchr(token,' ') ? "" : "Bearer ";
    int n=snprintf(request,sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\nProtocol-Version: 1\r\n"
        "Device-Id: %s\r\nClient-Id: %s\r\nAuthorization: %s%s\r\n\r\n",
        e->path,e->host,e->port,key,c->device,c->client,prefix,token);
    if (n<0 || (size_t)n>=sizeof(request)) { wipe(request,sizeof(request)); return fail("handshake request too large"); }
    int r=socket_write(w,request,(size_t)n); wipe(request,sizeof(request));
    if (r) return -1;
    char response[MAX_HTTP+1]; size_t len=0;
    while (len<MAX_HTTP) {
        if (socket_read(w,response+len,1)) return -1;
        ++len;
        if (len>=4 && !memcmp(response+len-4,"\r\n\r\n",4)) break;
    }
    if (len==MAX_HTTP) return fail("HTTP headers too large");
    response[len]=0;
    if (memchr(response,0,len)) return fail("invalid HTTP response");
    char *line=response, *end=strstr(line,"\r\n");
    if (!end || strncmp(line,"HTTP/1.1 101",12) || (end-line>12 && line[12]!=' ')) return fail("WebSocket upgrade rejected");
    line=end+2; int upgrade=0, connection=0, accept=0;
    while ((end=strstr(line,"\r\n")) && end!=line) {
        *end=0;
        char *colon=strchr(line,':');
        if (!colon || line[0]==' ' || line[0]=='\t') return fail("invalid HTTP header");
        *colon=0; char *value=colon+1;
        while (*value==' ' || *value=='\t') ++value;
        char *v_end=value+strlen(value);
        while (v_end>value && (v_end[-1]==' ' || v_end[-1]=='\t')) *--v_end=0;
        if (!strcasecmp(line,"Upgrade")) { if (upgrade || strcasecmp(value,"websocket")) return fail("invalid Upgrade header"); upgrade=1; }
        else if (!strcasecmp(line,"Connection")) { if (connection || !token_contains(value,"Upgrade")) return fail("invalid Connection header"); connection=1; }
        else if (!strcasecmp(line,"Sec-WebSocket-Accept")) { if (accept || strcmp(value,expect)) return fail("invalid WebSocket accept"); accept=1; }
        else if (!strcasecmp(line,"Sec-WebSocket-Extensions") || !strcasecmp(line,"Sec-WebSocket-Protocol")) return fail("unrequested WebSocket extension");
        line=end+2;
    }
    return upgrade && connection && accept ? 0 : fail("incomplete WebSocket upgrade");
}
static int send_frame_body(struct wire *w, unsigned opcode, const uint8_t *data, size_t len) {
    if (len>MAX_JSON || opcode>15) return fail("outgoing frame invalid");
    uint8_t header[14], mask[4]; size_t h=0;
    if (random_bytes(mask,sizeof(mask))) return -1;
    header[h++]=(uint8_t)(0x80|opcode);
    if (len<126) header[h++]=(uint8_t)(0x80|len);
    else if (len<=65535) { header[h++]=0xfe; header[h++]=(uint8_t)(len>>8); header[h++]=(uint8_t)len; }
    else { header[h++]=0xff; for (int i=7;i>=0;--i) header[h++]=(uint8_t)((uint64_t)len>>(8*i)); }
    memcpy(header+h,mask,4); h+=4;
    if (socket_write(w,header,h)) return -1;
    uint8_t block[1024]; size_t pos=0;
    while (pos<len) {
        size_t n=len-pos; if (n>sizeof(block)) n=sizeof(block);
        for (size_t i=0;i<n;++i) block[i]=data[pos+i]^mask[(pos+i)%4];
        if (socket_write(w,block,n)) return -1;
        pos+=n;
    }
    return 0;
}
static int send_frame(struct wire *w,unsigned opcode,const uint8_t *data,size_t len) {
    if(w->incomplete)return -1;
    if(session_keepalive_before_frame(w))return -1;
    ++w->writing;int result=send_frame_body(w,opcode,data,len);--w->writing;if(result)w->incomplete=1;return result;
}
static int receive_message_body(struct wire *w, struct message *m) {
    unsigned fragmented=0; m->len=0; m->opcode=0;
    for (;;) {
        uint8_t h[2]; if (socket_read(w,h,2)) return -1;
        unsigned fin=h[0]>>7, op=h[0]&15; uint64_t n=h[1]&127;
        if ((h[0]&0x70) || (h[1]&0x80)) return fail("unsupported or masked server frame");
        if (n==126) { uint8_t b[2]; if(socket_read(w,b,2))return -1; n=(unsigned)b[0]*256+b[1]; if(n<126)return fail("noncanonical frame length"); }
        else if(n==127) { uint8_t b[8]; if(socket_read(w,b,8))return -1; n=0; for(int i=0;i<8;++i)n=(n<<8)|b[i]; if((b[0]&128)||n<65536)return fail("noncanonical frame length"); }
        if(op>=8) {
            uint8_t ctl[125];
            if(!fin || n>125 || (op!=8 && op!=9 && op!=10) || (op==8 && n==1)) return fail("invalid control frame");
            if(socket_read(w,ctl,(size_t)n))return -1;
            if(op==9) { if(send_frame(w,10,ctl,(size_t)n))return -1; continue; }
            if(op==10)continue;
            if(n>=2) {
                unsigned code=(unsigned)ctl[0]*256+ctl[1];
                if(!((code>=1000&&code<=1014&&code!=1004&&code!=1005&&code!=1006)||
                     (code>=3000&&code<=4999))||!utf8_valid(ctl+2,(size_t)n-2))return fail("invalid close payload");
            }
            (void)send_frame(w,8,ctl,(size_t)n);
            return fail("server closed before completion");
        }
        if(op!=0 && op!=1 && op!=2)return fail("invalid data opcode");
        if(op==0 ? !fragmented : fragmented)return fail("invalid continuation sequence");
        if(op) { fragmented=op; m->opcode=op; }
        if(n>MAX_JSON-m->len)return fail("incoming message too large");
        if(socket_read(w,m->data+m->len,(size_t)n))return -1;
        m->len+=(size_t)n;
        if(fin) { m->data[m->len]=0; return 0; }
    }
}
static int receive_message(struct wire *w,struct message *m) {
    ++w->receiving;w->mcp_deferred=0;int result=receive_message_body(w,m);--w->receiving;return result;
}
static int send_json(struct wire *w, struct json_object *j) {
    if(!j)return fail("JSON allocation failed");
    const char *s=json_object_to_json_string_ext(j,0);
    int r=s?send_frame(w,1,(const uint8_t *)s,strlen(s)):fail("JSON serialization failed");
    json_object_put(j); return r;
}
static void add_string(struct json_object *j,const char *k,const char *s) { json_object_object_add(j,k,json_object_new_string(s)); }
/* Reply semantics are advisory and scoped to one client-created request. They
 * never authorize recording on their own, and never survive a new request or
 * connection. No server field supplies a duration or executable action. */
static void dialogue_reset(struct wire *w) {
    wipe(w->turn_id,sizeof(w->turn_id));w->dialogue_seen=w->dialogue_invalid=0;w->dialogue_state=DIALOGUE_FALLBACK;
    w->command_prompt=COMMAND_PROMPT_NONE;w->response_audio=0;w->followup_request=w->quiet_completed=0;
}
static int dialogue_begin(struct wire *w) {
    dialogue_reset(w);if(!followup_seconds)return 0;
    uint8_t id[16];if(random_bytes(id,sizeof(id)))return -1;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<sizeof(id);++i){w->turn_id[i*2]=hex[id[i]>>4];w->turn_id[i*2+1]=hex[id[i]&15];}
    wipe(id,sizeof(id));return 0;
}
static void dialogue_add_id(struct wire *w,struct json_object *j) {
    if(w->turn_id[0])add_string(j,"turn_id",w->turn_id);
}
/* json-c accepts duplicate keys. This tiny flat-message framing check rejects
 * repeated, escaped or unknown keys independently of its decoded values. */
static int dialogue_flat_keys(const uint8_t *raw,size_t n) {
    if(!n||n>512)return 0;
    unsigned bits=0,depth=0;
    for(size_t i=0;i<n;++i) {
        unsigned c=raw[i];
        if(c=='{'||c=='['){if(c!='{'||depth++)return 0;continue;}
        if(c=='}'||c==']'){if(c!='}'||depth!=1)return 0;--depth;continue;}
        if(c!='"')continue;
        size_t start=++i;int escaped=0;
        while(i<n&&raw[i]!='"'){if(raw[i]=='\\'){escaped=1;if(++i==n)return 0;}++i;}
        if(i==n)return 0;
        size_t end=i,next=i+1;while(next<n&&strchr(" \t\r\n",raw[next]))++next;
        if(next<n&&raw[next]==':') {
            if(depth!=1||escaped)return 0;
            unsigned bit=0;size_t length=end-start;
            if(length==4&&!memcmp(raw+start,"type",4))bit=1;
            else if(length==5&&!memcmp(raw+start,"state",5))bit=2;
            else if(length==10&&!memcmp(raw+start,"session_id",10))bit=4;
            else if(length==7&&!memcmp(raw+start,"turn_id",7))bit=8;
            else if(length==12&&!memcmp(raw+start,"local_prompt",12))bit=16;
            if(!bit||(bits&bit))return 0;bits|=bit;
        }
    }
    return !depth&&(bits==15||bits==31);
}
static void dialogue_receive(struct wire *w,struct json_object *j,const char *sid,const uint8_t *raw,size_t n,int started,int audio) {
    const char *session=string_field(j,"session_id"),*turn=string_field(j,"turn_id"),*state=string_field(j,"state");
    if(!w->turn_id[0]||!session||strcmp(session,sid)||!turn||strcmp(turn,w->turn_id)) {
        puts("{\"event\":\"dialogue_ignored\",\"reason\":\"stale_or_unmatched_request\"}");return;
    }
    if(w->dialogue_seen++||!dialogue_flat_keys(raw,n)||!state||
       (strcmp(state,"close")&&strcmp(state,"listen_only")&&(!started||!audio)))w->dialogue_invalid=1;
    enum dialogue_state next=DIALOGUE_FALLBACK;
    if(state&&!strcmp(state,"await_reply"))next=DIALOGUE_AWAIT_REPLY;
    else if(state&&!strcmp(state,"allow_followup"))next=DIALOGUE_ALLOW_FOLLOWUP;
    else if(state&&!strcmp(state,"close"))next=DIALOGUE_CLOSE;
    else if(state&&!strcmp(state,"listen_only")) {
        next=DIALOGUE_LISTEN_ONLY;
        if(!local_wake_enabled||!followup_seconds||!w->quiet_followup_capable||!w->followup_request||started||audio||w->response_audio)w->dialogue_invalid=1;
    } else w->dialogue_invalid=1;
    struct json_object *prompt_field=NULL;
    if(json_object_object_get_ex(j,"local_prompt",&prompt_field)) {
        const char *prompt=string_field(j,"local_prompt");
        if(!w->command_feedback_capable||next!=DIALOGUE_CLOSE||started||audio||w->response_audio||!prompt)w->dialogue_invalid=1;
        else if(!strcmp(prompt,"farewell"))w->command_prompt=COMMAND_PROMPT_FAREWELL;
        else if(!strcmp(prompt,"key"))w->command_prompt=COMMAND_PROMPT_KEY;
        else w->dialogue_invalid=1;
    }
    if(w->dialogue_invalid)w->command_prompt=COMMAND_PROMPT_NONE;
    w->dialogue_state=w->dialogue_invalid?DIALOGUE_FALLBACK:next;
    printf("{\"event\":\"dialogue_metadata\",\"valid\":%s,\"state\":\"%s\"}\n",w->dialogue_invalid?"false":"true",
        w->dialogue_state==DIALOGUE_AWAIT_REPLY?"await_reply":w->dialogue_state==DIALOGUE_ALLOW_FOLLOWUP?"allow_followup":w->dialogue_state==DIALOGUE_CLOSE?"close":w->dialogue_state==DIALOGUE_LISTEN_ONLY?"listen_only":"fallback");
}
/* A prompt is consumed only after a completed stop-only turn. Semantics alone
 * never select speech, and old/duplicate/audio-bearing responses cannot cue. */
static enum command_prompt dialogue_take_command_prompt(struct wire *w) {
    enum command_prompt prompt=w->command_prompt;w->command_prompt=COMMAND_PROMPT_NONE;
    if(!local_wake_enabled||!w->command_feedback_capable||!w->turn_id[0]||w->dialogue_seen!=1||w->dialogue_invalid||
       w->dialogue_state!=DIALOGUE_CLOSE||w->response_audio)return COMMAND_PROMPT_NONE;
    return prompt;
}
static int dialogue_quiet_completed(const struct wire *w) {
    return local_wake_enabled&&followup_seconds&&w->quiet_followup_capable&&w->followup_request&&w->quiet_completed&&
        w->turn_id[0]&&w->dialogue_seen==1&&!w->dialogue_invalid&&w->dialogue_state==DIALOGUE_LISTEN_ONLY&&!w->response_audio;
}
static unsigned dialogue_wait_ms(const struct wire *w) {
    if(!followup_seconds)return 0;
    if(!w->dialogue_invalid&&w->dialogue_state==DIALOGUE_CLOSE)return 0;
    if(!w->dialogue_invalid&&w->dialogue_state==DIALOGUE_AWAIT_REPLY)return 20000;
    return followup_seconds*1000;
}
/* One main-thread writer. Keepalive polling uses direct nonblocking send,
 * never socket_write/ready_fd, and never inserts bytes into a normal frame.
 * A partial keepalive must complete before any subsequent WebSocket frame. */
static void session_keepalive_unbind(void) {wipe(&session_keepalive,sizeof(session_keepalive));}
static int session_keepalive_bind(struct wire *w,const char *sid) {
    session_keepalive_unbind();
    (void)sid;
    /* RFC 6455 Ping keeps any standards-compliant server alive. No private
     * listen/keepalive application message is required. */
    static const char payload[]="s12-ping";
    memcpy(session_keepalive.payload,payload,sizeof(payload)-1);session_keepalive.payload_bytes=sizeof(payload)-1;
    session_keepalive.wire=w;session_keepalive.next=now_ms()+5000;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *interval=getenv("S12_TEST_KEEPALIVE_MS");
    if(interval){int value=atoi(interval);if(value>=10&&value<=5000)session_keepalive.next=now_ms()+value;}
#endif
    return 0;
}
static void session_keepalive_fail(const char *stage) {
    session_keepalive.failed=1;(void)fail(stage);
}
static void session_keepalive_flush(void) {
    if(session_keepalive.failed||session_keepalive.sent>=session_keepalive.bytes)return;
    if(!remaining(session_keepalive.deadline)){session_keepalive_fail("socket keepalive deadline expired");return;}
    ssize_t n=send(session_keepalive.wire->fd,session_keepalive.frame+session_keepalive.sent,
                   session_keepalive.bytes-session_keepalive.sent,MSG_DONTWAIT);
    if(n>0)session_keepalive.sent+=(size_t)n;
    else if(!n)session_keepalive_fail("socket peer closed during keepalive");
    else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)session_keepalive_fail("socket keepalive write failed");
    if(session_keepalive.sent==session_keepalive.bytes)session_keepalive.sent=session_keepalive.bytes=0;
}
static void session_keepalive_poll(void) {
    struct wire *w=session_keepalive.wire;
    if(!w||w->fd<0||w->writing||session_keepalive.failed||stopping||panel_interrupted)return;
    if(w->incomplete){session_keepalive.failed=1;return;}
    device_mcp_poll_input(w);if(session_keepalive.failed)return;
    if(followup_capture_deadline) {
        uint8_t byte;ssize_t n=recv(w->fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
        if(!n){session_keepalive_fail("socket peer closed during followup");return;}
        if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){session_keepalive_fail("socket peer closed during followup");return;}
        if((n>0&&(byte&0x8f)==0x88)||(w->pos<w->used&&(w->pending[w->pos]&0x8f)==0x88)){session_keepalive_fail("socket peer closed during followup");return;}
    }
    if(!session_keepalive.bytes) {
        if(now_ms()<session_keepalive.next)return;
        uint8_t mask[4];if(random_bytes(mask,sizeof(mask))){session_keepalive.failed=1;return;}
        size_t n=session_keepalive.payload_bytes,at=0;
        session_keepalive.frame[at++]=0x89;
        if(n<126)session_keepalive.frame[at++]=(uint8_t)(0x80|n);
        else {session_keepalive.frame[at++]=0xfe;session_keepalive.frame[at++]=(uint8_t)(n>>8);session_keepalive.frame[at++]=(uint8_t)n;}
        memcpy(session_keepalive.frame+at,mask,4);at+=4;
        for(size_t i=0;i<n;++i)session_keepalive.frame[at+i]=session_keepalive.payload[i]^mask[i%4];
        session_keepalive.bytes=at+n;session_keepalive.sent=0;session_keepalive.deadline=now_ms()+1000;
        int interval=5000;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
        const char *test=getenv("S12_TEST_KEEPALIVE_MS");if(test){int value=atoi(test);if(value>=10&&value<=5000)interval=value;}
#endif
        session_keepalive.next=now_ms()+interval;
    }
    session_keepalive_flush();
}
static int session_keepalive_before_frame(struct wire *w) {
    if(session_keepalive.wire!=w)return 0;
    if(session_keepalive.failed)return -1;
    ++w->writing;
    while(session_keepalive.bytes&&!stopping&&!panel_interrupted) {
        if(!remaining(w->deadline)){session_keepalive_fail("socket keepalive business deadline expired");break;}
        session_keepalive_flush();if(session_keepalive.failed)break;
        if(session_keepalive.bytes) {
            panel_poll();if(stopping||panel_interrupted)break;
            struct pollfd p={w->fd,POLLOUT,0};int r=poll(&p,1,20);
            if(r<0&&errno!=EINTR){session_keepalive_fail("socket keepalive poll failed");break;}
        }
    }
    --w->writing;
    return session_keepalive.failed||session_keepalive.bytes?-1:0;
}
static int send_hello(struct wire *w) {
    struct json_object *j=json_object_new_object(), *a=json_object_new_object();
    if(!j||!a) { if(j)json_object_put(j);if(a)json_object_put(a);return fail("JSON allocation failed"); }
    add_string(j,"type","hello"); json_object_object_add(j,"version",json_object_new_int(1)); add_string(j,"transport","websocket");
    add_string(a,"format","opus"); json_object_object_add(a,"sample_rate",json_object_new_int(16000));
    json_object_object_add(a,"channels",json_object_new_int(1)); json_object_object_add(a,"frame_duration",json_object_new_int(60));
    json_object_object_add(j,"audio_params",a);
    if(followup_seconds||panel_enabled) {
        extern struct json_object *json_object_new_boolean(int);
        struct json_object *features=json_object_new_object();if(!features){json_object_put(j);return fail("JSON allocation failed");}
        if(panel_enabled)json_object_object_add(features,"mcp",json_object_new_boolean(1));
        if(followup_seconds) {
            json_object_object_add(features,"dialogue_state",json_object_new_boolean(1));
            if(local_wake_enabled) {
                json_object_object_add(features,"local_command_feedback",json_object_new_boolean(1));
                json_object_object_add(features,"quiet_followup",json_object_new_boolean(1));
                json_object_object_add(features,"capture_diagnostics",json_object_new_boolean(1));
            }
        }
        json_object_object_add(j,"features",features);
    }
    int result=send_json(w,j);w->command_feedback_capable=!result&&followup_seconds&&local_wake_enabled;
    w->quiet_followup_capable=w->command_feedback_capable;
    w->capture_diagnostics_capable=w->quiet_followup_capable;return result;
}
/* Standard device MCP runs on the existing voice WebSocket only.
 * Protocol discovery is pure JSON; panel IO and all side effects stay main. */
extern struct json_object *json_object_new_boolean(int);
extern struct json_object *json_object_get(struct json_object *);
extern int64_t json_object_get_int64(struct json_object *);
extern int json_object_object_length(struct json_object *);
static uint64_t mcp_voice_epoch;
struct mcp_route {struct wire *wire;uint64_t voice_epoch;};
static struct {
    struct mcp_route route;struct json_object *id;char nonce[33];unsigned volume,slot;
    int dir;int64_t deadline;int active;
} volume_rpc;
static int mcp_servicing;
static int mcp_id_valid(struct json_object *id) {
    if(!id)return 0;
    if(json_object_is_type(id,json_type_int)){int64_t n=json_object_get_int64(id);return n>=-INT64_C(9007199254740991)&&n<=INT64_C(9007199254740991);}
    if(!json_object_is_type(id,json_type_string))return 0;
    const char *s=json_object_get_string(id);int n=json_object_get_string_len(id);
    if(!s||n<1||n>128||strlen(s)!=(size_t)n)return 0;
    const char *raw=json_object_to_json_string_ext(id,0);return raw&&strlen(raw)<160;
}
static struct json_object *mcp_envelope(struct json_object *id,struct json_object *result,int code,const char *message) {
    struct json_object *outer=json_object_new_object(),*rpc=json_object_new_object();
    if(!outer||!rpc){if(outer)json_object_put(outer);if(rpc)json_object_put(rpc);if(result)json_object_put(result);return NULL;}
    add_string(outer,"type","mcp");add_string(rpc,"jsonrpc","2.0");json_object_object_add(rpc,"id",id?json_object_get(id):NULL);
    if(code){struct json_object *error=json_object_new_object();if(!error){json_object_put(outer);json_object_put(rpc);if(result)json_object_put(result);return NULL;}
        json_object_object_add(error,"code",json_object_new_int(code));add_string(error,"message",message);json_object_object_add(rpc,"error",error);if(result)json_object_put(result);}
    else json_object_object_add(rpc,"result",result);
    json_object_object_add(outer,"payload",rpc);return outer;
}
static struct json_object *mcp_text_result(const char *text,int error) {
    struct json_object *j=json_object_new_object();if(!j)return NULL;
    /* This tiny fixed wrapper uses the public parser rather than adding an
     * ABI dependency on json-c's array layout. Text itself is JSON escaped. */
    const char *base="{\"content\":[{\"type\":\"text\",\"text\":\"\"}],\"isError\":false}";
    json_object_put(j);j=parse_json(base,strlen(base));if(!j)return NULL;
    extern struct json_object *json_object_array_get_idx(struct json_object *,int);
    struct json_object *content=field(j,"content",json_type_array),*item=json_object_array_get_idx(content,0);
    if(!item){json_object_put(j);return NULL;}add_string(item,"text",text);
    json_object_object_add(j,"isError",json_object_new_boolean(error));return j;
}
static struct json_object *mcp_static_answer(struct json_object *outer,unsigned *stage,int enabled,int *handled) {
    *handled=1;struct json_object *rpc=field(outer,"payload",json_type_object),*id=NULL;
    if(rpc)(void)json_object_object_get_ex(rpc,"id",&id);
    const char *version=string_field(rpc,"jsonrpc"),*method=string_field(rpc,"method");
    int valid_id=mcp_id_valid(id);
    if(!rpc||!version||strcmp(version,"2.0")||!method||!*method||strlen(method)>128||json_object_object_length(rpc)>4)
        return mcp_envelope(valid_id?id:NULL,NULL,-32600,"Invalid Request");
    if(!strcmp(method,"notifications/initialized")&&!id){if(*stage)*stage=2;return NULL;}
    if(!valid_id)return mcp_envelope(NULL,NULL,-32600,"Invalid Request");
    struct json_object *params=field(rpc,"params",json_type_object);
    if(!params)return mcp_envelope(id,NULL,-32602,"Invalid params");
    if(!strcmp(method,"initialize")) {
        const char *protocol=string_field(params,"protocolVersion");
        if(!protocol||strcmp(protocol,"2024-11-05"))return mcp_envelope(id,NULL,-32602,"Unsupported protocol version");
        const char *text="{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"s12-device\",\"version\":\"1\"}}";
        *stage=1;return mcp_envelope(id,parse_json(text,strlen(text)),0,NULL);
    }
    if(!*stage)return mcp_envelope(id,NULL,-32002,"Server not initialized");
    if(!strcmp(method,"tools/list")) {
        if(json_object_object_length(params))return mcp_envelope(id,NULL,-32602,"Invalid params");
        const char *tools=enabled?
            "{\"tools\":[{\"name\":\"self.get_device_status\",\"description\":\"Read current speaker volume\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},{\"name\":\"self.audio_speaker.set_volume\",\"description\":\"Set speaker volume from 0 to 100; microphone privacy is unchanged\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},\"required\":[\"volume\"],\"additionalProperties\":false}}]}":"{\"tools\":[]}";
        return mcp_envelope(id,parse_json(tools,strlen(tools)),0,NULL);
    }
    if(strcmp(method,"tools/call"))return mcp_envelope(id,NULL,-32601,"Method not found");
    if(!enabled)return mcp_envelope(id,NULL,-32601,"Device controls unavailable");
    *handled=0;return NULL;
}
static int mcp_route_valid(const struct mcp_route *route) {
    return route->wire&&route->wire->fd>=0&&!route->wire->incomplete&&route->wire->mcp_epoch==route->voice_epoch;
}
static int mcp_reply(const struct mcp_route *route,struct json_object *response) {
    if(!response)return -1;
    if(!mcp_route_valid(route)){json_object_put(response);return -1;}
    return send_json(route->wire,response);
}
static int mcp_read_private(int dir,const char *name,char *raw,size_t cap) {
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?-2:-1;
    struct stat st;int good=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&
        (st.st_mode&07777)==0600&&st.st_size>0&&st.st_size<(off_t)cap;
    size_t at=0;while(good&&at<(size_t)st.st_size){ssize_t n=read(fd,raw+at,(size_t)st.st_size-at);if(n<0&&errno==EINTR)continue;if(n<=0){good=0;break;}at+=(size_t)n;}
    if(good){uint8_t extra;ssize_t n;do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);if(n)good=0;}
    close(fd);if(!good||memchr(raw,0,at))return -1;raw[at]=0;return (int)at;
}
static int mcp_panel_status(int dir,struct panel_control *current) {
    char raw[160];int n=mcp_read_private(dir,"panel.control",raw,sizeof(raw));return n>0?panel_parse(raw,current):-1;
}
static int mcp_number(const char **p,uint64_t max,uint64_t *value) {
    const char *s=*p;if(*s<'0'||*s>'9')return -1;uint64_t n=0;
    do{unsigned digit=(unsigned)(*s++-'0');if(n>(max-digit)/10)return -1;n=n*10+digit;}while(*s>='0'&&*s<='9');
    *p=s;*value=n;return 0;
}
static int mcp_volume_ack(unsigned *sequence) {
    char raw[192];int n=mcp_read_private(volume_rpc.dir,"panel-volume.ack",raw,sizeof(raw));if(n==-2)return 0;if(n<0)return -1;
    if(n<36||strncmp(raw,"v1 ",3))return 0;
    for(unsigned i=3;i<35;++i)if(!((raw[i]>='0'&&raw[i]<='9')||(raw[i]>='a'&&raw[i]<='f')))return 0;
    if(raw[35]!=' '||memcmp(raw+3,volume_rpc.nonce,32))return 0;
    const char *p=raw+36;
    if(!strncmp(p,"failed ",7))return -1;
    if(strncmp(p,"ok ",3))return -1;p+=3;uint64_t volume,gain,seq;
    if(mcp_number(&p,100,&volume)||*p++!=' '||mcp_number(&p,200,&gain)||*p++!=' '||mcp_number(&p,UINT32_MAX,&seq)||*p++!='\n'||*p)return -1;
    if(volume!=volume_rpc.volume||gain!=volume_rpc.volume*2)return -1;*sequence=(unsigned)seq;return 1;
}
static void mcp_remove_request(void) {
    if(volume_rpc.dir<0)return;
    char raw[192];int n=mcp_read_private(volume_rpc.dir,"panel-volume.request",raw,sizeof(raw));
    if(n>=36&&!strncmp(raw,"v1 ",3)&&!memcmp(raw+3,volume_rpc.nonce,32)&&raw[35]==' ')(void)unlinkat(volume_rpc.dir,"panel-volume.request",0);
}
static void mcp_pending_clear(void) {
    if(!volume_rpc.active)return;mcp_remove_request();if(volume_rpc.id)json_object_put(volume_rpc.id);if(volume_rpc.dir>=0)close(volume_rpc.dir);
    wipe(&volume_rpc,sizeof(volume_rpc));volume_rpc.dir=-1;
}
static void device_mcp_cancel(struct wire *w) {
    if(volume_rpc.active&&volume_rpc.route.wire==w)mcp_pending_clear();
}
static int mcp_request_volume(int dir,const char *nonce,unsigned volume,int64_t deadline) {
    int old=openat(dir,"panel-volume.request",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(old>=0){close(old);return -1;}if(errno!=ENOENT)return -1;
    char tmp[80];snprintf(tmp,sizeof(tmp),".panel-volume.request.%s",nonce);
    int fd=openat(dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return -1;
    char raw[128];int n=snprintf(raw,sizeof(raw),"v1 %s %u %lld\n",nonce,volume,(long long)deadline);int good=n>0&&(size_t)n<sizeof(raw);size_t at=0;
    while(good&&at<(size_t)n){ssize_t r=write(fd,raw+at,(size_t)n-at);if(r<0&&errno==EINTR)continue;if(r<=0){good=0;break;}at+=(size_t)r;}
    if(close(fd))good=0;if(good&&renameat(dir,tmp,dir,"panel-volume.request"))good=0;if(!good)(void)unlinkat(dir,tmp,0);return good?0:-1;
}
static int device_mcp_receive(struct wire *w,struct json_object *outer) {
    struct mcp_route route={w,w?w->mcp_epoch:0};if(!mcp_route_valid(&route))return -1;int handled=0;unsigned stage=w?w->mcp_initialized:1;
    struct json_object *answer=mcp_static_answer(outer,&stage,panel_enabled,&handled);if(w)w->mcp_initialized=stage;
    if(handled)return answer?mcp_reply(&route,answer):0;
    struct json_object *rpc=field(outer,"payload",json_type_object),*id=NULL;(void)json_object_object_get_ex(rpc,"id",&id);
    struct json_object *params=field(rpc,"params",json_type_object),*arguments=field(params,"arguments",json_type_object);const char *name=string_field(params,"name");
    if(!name||!arguments||json_object_object_length(params)!=2)return mcp_reply(&route,mcp_envelope(id,NULL,-32602,"Invalid params"));
    if(!strcmp(name,"self.get_device_status")) {
        if(json_object_object_length(arguments))return mcp_reply(&route,mcp_envelope(id,NULL,-32602,"Invalid params"));
        int dir=startup_directory(0);struct panel_control current;int good=dir>=0&&!mcp_panel_status(dir,&current);if(dir>=0)close(dir);
        char text[96];snprintf(text,sizeof(text),"{\"audio_speaker\":{\"volume\":%u}}",good?current.gain/2:0);
        return mcp_reply(&route,mcp_envelope(id,mcp_text_result(good?text:"panel_state_unavailable",!good),0,NULL));
    }
    if(strcmp(name,"self.audio_speaker.set_volume"))return mcp_reply(&route,mcp_envelope(id,NULL,-32602,"Unknown tool"));
    struct json_object *value=field(arguments,"volume",json_type_int);int64_t target=value?json_object_get_int64(value):-1;
    if(!value||target<0||target>100||json_object_object_length(arguments)!=1)return mcp_reply(&route,mcp_envelope(id,NULL,-32602,"Invalid params"));
    const char *key=json_object_to_json_string_ext(id,0);unsigned slot=0;
    if(w){int found=0;
        for(unsigned i=0;i<8;++i)if(w->mcp_cache[i].id[0]&&!strcmp(w->mcp_cache[i].id,key)){
            found=1;struct mcp_cached_volume *cached=&w->mcp_cache[i];
            if(cached->volume!=(int)target)return mcp_reply(&route,mcp_envelope(id,NULL,-32600,"Request id reused"));
            if(!cached->result)return 0;
            return mcp_reply(&route,mcp_envelope(id,mcp_text_result(cached->result>0?"true":cached->reason,cached->result<0),0,NULL));}
        (void)found;
        slot=w->mcp_cache_next++%8;
        if(volume_rpc.active&&volume_rpc.route.wire==w&&slot==volume_rpc.slot)slot=w->mcp_cache_next++%8;
        memset(&w->mcp_cache[slot],0,sizeof(w->mcp_cache[slot]));strcpy(w->mcp_cache[slot].id,key);w->mcp_cache[slot].volume=(int)target;
    }
    if(volume_rpc.active){if(w){w->mcp_cache[slot].result=-1;w->mcp_cache[slot].reason="volume_request_busy";}return mcp_reply(&route,mcp_envelope(id,mcp_text_result("volume_request_busy",1),0,NULL));}
    int dir=startup_directory(0);uint8_t random[16];char nonce[33];int64_t deadline=now_ms()+1500;
    if(dir<0||random_bytes(random,sizeof(random))){if(dir>=0)close(dir);if(w){w->mcp_cache[slot].result=-1;w->mcp_cache[slot].reason="panel_request_failed";}return mcp_reply(&route,mcp_envelope(id,mcp_text_result("panel_request_failed",1),0,NULL));}
    for(unsigned i=0;i<16;++i)snprintf(nonce+i*2,3,"%02x",random[i]);wipe(random,sizeof(random));
    if(mcp_request_volume(dir,nonce,(unsigned)target,deadline)){close(dir);if(w){w->mcp_cache[slot].result=-1;w->mcp_cache[slot].reason="panel_request_failed";}return mcp_reply(&route,mcp_envelope(id,mcp_text_result("panel_request_failed",1),0,NULL));}
    volume_rpc.route=route;volume_rpc.id=json_object_get(id);strcpy(volume_rpc.nonce,nonce);volume_rpc.volume=(unsigned)target;volume_rpc.slot=slot;
    volume_rpc.dir=dir;volume_rpc.deadline=deadline;volume_rpc.active=1;return 0;
}
static void device_mcp_service(void) {
    if(mcp_servicing)return;mcp_servicing=1;
    if(volume_rpc.active) {
        if(!mcp_route_valid(&volume_rpc.route))mcp_pending_clear();
        else if(!volume_rpc.route.wire||!volume_rpc.route.wire->writing) {
            unsigned seq=0;int result=remaining(volume_rpc.deadline)?mcp_volume_ack(&seq):-1;
            if(result) {
                struct panel_control current;int good=result>0&&!mcp_panel_status(volume_rpc.dir,&current)&&current.seq>=seq&&current.gain==volume_rpc.volume*2;
                if(good){panel_next_poll=0;panel_poll_controls();}
                if(volume_rpc.route.wire){struct mcp_cached_volume *cache=&volume_rpc.route.wire->mcp_cache[volume_rpc.slot];cache->result=good?1:-1;cache->reason="volume_update_unconfirmed";}
                (void)mcp_reply(&volume_rpc.route,mcp_envelope(volume_rpc.id,mcp_text_result(good?"true":"volume_update_unconfirmed",!good),0,NULL));mcp_pending_clear();
            }
        }
    }
    mcp_servicing=0;
}
/* Standard servers emit complete JSON MCP frames. While local buffered playback,
 * drain or a follow-up window owns main, consume only those complete frames.
 * Every other frame is retained verbatim for the normal receiver. */
static int mcp_input_busy;
static void device_mcp_poll_input(struct wire *w) {
    if(mcp_input_busy||w->receiving||w->writing||w->incomplete||w->mcp_deferred||w->fd<0)return;
    mcp_input_busy=1;
    for(unsigned messages=0;messages<4;++messages) {
        if(w->pos){size_t left=w->used-w->pos;memmove(w->pending,w->pending+w->pos,left);w->used=left;w->pos=0;}
        if(w->used<sizeof(w->pending)) {
            ssize_t n=recv(w->fd,w->pending+w->used,sizeof(w->pending)-w->used,MSG_DONTWAIT);
            if(n>0)w->used+=(size_t)n;
            else if(!n){if(w->used)w->mcp_deferred=1;else session_keepalive_fail(followup_capture_deadline?"socket peer closed during followup":"socket peer closed during MCP poll");break;}
            else if(errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK){if(w->used)w->mcp_deferred=1;else session_keepalive_fail("socket read failed during MCP poll");break;}
        }
        if(w->used<2)break;
        unsigned op=w->pending[0]&15;size_t header=2,bytes=w->pending[1]&127;
        if((w->pending[0]&0xf0)!=0x80||(w->pending[1]&0x80)||bytes==127){w->mcp_deferred=1;break;}
        if(bytes==126){if(w->used<4)break;bytes=(size_t)w->pending[2]*256+w->pending[3];header=4;if(bytes<126){w->mcp_deferred=1;break;}}
        if(bytes>sizeof(w->pending)-header){w->mcp_deferred=1;break;}
        if(w->used<header+bytes)break;
        if(op==9||op==10) {
            if(bytes>125){w->mcp_deferred=1;break;}
            if(op==9&&send_frame(w,10,w->pending+header,bytes)){session_keepalive.failed=1;break;}
            w->pos=header+bytes;continue;
        }
        if(op!=1){w->mcp_deferred=1;break;}
        struct json_object *j=parse_json(w->pending+header,bytes);const char *type=string_field(j,"type");
        if(!type||strcmp(type,"mcp")){if(j)json_object_put(j);w->mcp_deferred=1;break;}
        w->pos=header+bytes;int result=device_mcp_receive(w,j);json_object_put(j);
        if(result<0){session_keepalive.failed=1;break;}
    }
    mcp_input_busy=0;
}
static int receive_hello(struct wire *w,char sid[128],int *sample_rate) {
    struct message *m=malloc(sizeof(*m)); if(!m)return fail("message allocation failed");
    int rc=-1;
    if(receive_message(w,m))goto out;
    if(m->opcode!=1){fail("expected JSON server hello");goto out;}
    struct json_object *j=parse_json(m->data,m->len);
    if(!j){fail("invalid server hello JSON");goto out;}
    const char *type=string_field(j,"type"), *transport=string_field(j,"transport"), *session=string_field(j,"session_id");
    struct json_object *a=field(j,"audio_params",json_type_object), *rate=field(a,"sample_rate",json_type_int), *channels=field(a,"channels",json_type_int);
    const char *format=string_field(a,"format");
    int r=rate?json_object_get_int(rate):24000;
    struct json_object *present=NULL;
    int invalid_audio=json_object_object_get_ex(j,"audio_params",&present)&&!a;
    if(a) {
        invalid_audio|=json_object_object_get_ex(a,"sample_rate",&present)&&!rate;
        invalid_audio|=json_object_object_get_ex(a,"channels",&present)&&!channels;
        invalid_audio|=json_object_object_get_ex(a,"format",&present)&&!format;
    }
    int invalid_session=json_object_object_get_ex(j,"session_id",&present)&&!session;
    if(!type||strcmp(type,"hello")||!transport||strcmp(transport,"websocket")||invalid_audio||invalid_session||
       (session&&strlen(session)>=128)||(format&&strcmp(format,"opus"))||(channels&&json_object_get_int(channels)!=1)||
       (r!=8000&&r!=12000&&r!=16000&&r!=24000&&r!=48000)) fail("unsupported server hello");
    else {dialogue_reset(w);device_mcp_cancel(w);w->mcp_epoch=++mcp_voice_epoch;w->mcp_initialized=0;w->mcp_cache_next=0;wipe(w->mcp_cache,sizeof(w->mcp_cache));strcpy(sid,session?session:"");*sample_rate=r;rc=0;}
    json_object_put(j);
out: free(m);return rc;
}

#ifndef XIAOZHI_TEST_NO_ALSA
static int open_playback(struct audio *a) {
    const char *name=getenv("S12_PLAY_PCM"); if(!name)name="hw:0,1";
    if(strcmp(name,"hw:0,1")&&strcmp(name,"s12play"))return fail("playback PCM must be hw:0,1 or s12play");
    if(snd_pcm_open(&a->pcm,name,S12_PLAYBACK,S12_NONBLOCK)<0)return fail("ALSA playback open failed");
    snd_pcm_hw_params_t *h=NULL;snd_pcm_sw_params_t *sw=NULL; int rc=-1;
    snd_pcm_uframes_t period=960,buffer=15360;
    int dir=0,fmt=-1;unsigned ch=0,rate=0;
    if(snd_pcm_hw_params_malloc(&h)<0 || snd_pcm_hw_params_any(a->pcm,h)<0 ||
       snd_pcm_hw_params_set_access(a->pcm,h,S12_RW_INTERLEAVED)<0 ||
       snd_pcm_hw_params_set_format(a->pcm,h,S12_S32_LE)<0 ||
       snd_pcm_hw_params_set_channels(a->pcm,h,2)<0 ||
       snd_pcm_hw_params_set_rate(a->pcm,h,48000,0)<0 ||
       snd_pcm_hw_params_set_period_size_near(a->pcm,h,&period,&dir)<0 ||
       snd_pcm_hw_params_set_buffer_size_near(a->pcm,h,&buffer)<0 ||
       snd_pcm_hw_params(a->pcm,h)<0 ||
       snd_pcm_hw_params_get_format(h,&fmt)<0 || snd_pcm_hw_params_get_channels(h,&ch)<0 ||
       snd_pcm_hw_params_get_rate(h,&rate,&dir)<0 || fmt!=S12_S32_LE || ch!=2 || rate!=48000 ||
       snd_pcm_sw_params_malloc(&sw)<0 || snd_pcm_sw_params_current(a->pcm,sw)<0 ||
       snd_pcm_sw_params_set_start_threshold(a->pcm,sw,a->ready_prompt?period:(buffer<11520?buffer:11520))<0 ||
       snd_pcm_sw_params(a->pcm,sw)<0 || snd_pcm_prepare(a->pcm)<0) fail("ALSA exact playback setup failed");
    else if(!barge_active)rc=0;
    else {
        snd_pcm_uframes_t actual_buffer=0,actual_period=0;
        if(snd_pcm_get_params(a->pcm,&actual_buffer,&actual_period)<0)fail("ALSA playback parameters unavailable");
        else {barge_playback_settings((unsigned)actual_buffer,(unsigned)actual_period);rc=0;}
    }
    if(h)snd_pcm_hw_params_free(h);
    if(sw)snd_pcm_sw_params_free(sw);
    return rc;
}
static int play_samples(struct audio *a,const int16_t *mono,int count,int64_t deadline) {
    panel_poll();if(stopping||panel_interrupted)return -1;
    if(panel_enabled)a->gain=(int)panel.gain;
    if(!a->pcm&&open_playback(a))return -1;
    int32_t stereo[DECODE_FRAMES*2];
    for(int i=0;i<count;++i) {
        int32_t v=(int32_t)(((int64_t)mono[i]*a->gain*65536)/1000);
        stereo[2*i]=v;stereo[2*i+1]=v;
    }
    if(!a->ui_prompt)ui_feedback_overlay(stereo,count,a->gain);
    size_t off=0;
    while(off<(size_t)count) {
        panel_poll();if(stopping||panel_interrupted)return -1;
        if(!remaining(deadline))return fail("ALSA playback deadline expired");
        snd_pcm_sframes_t n=snd_pcm_writei(a->pcm,stereo+off*2,(snd_pcm_uframes_t)((size_t)count-off));
        if(n==-EAGAIN || n==0) { int waited=snd_pcm_wait(a->pcm,20);if(waited<0)return fail(waited==-EPIPE?"ALSA playback XRUN":"ALSA playback wait failed");continue; }
        if(n==-EPIPE)return fail("ALSA playback XRUN");
        if(n<0 || (size_t)n>(size_t)count-off)return fail("ALSA playback transfer failed");
        off+=(size_t)n;
    }
    return 0;
}
static int drain_playback(struct audio *a,int64_t deadline) {
    if(!a->pcm)return 0;
    /* Finish an overlay already started in the same stream before the final
     * hardware drain. No answer/song frames are replaced or replayed. */
    uint64_t feedback_tail=0;
    while(!a->ui_prompt&&ui_feedback.active&&!stopping&&!panel_interrupted) {
        unsigned end=(ui_feedback.active==1?S12_VOLUME_PROMPT_PCM_BYTES:S12_BUTTON_PROMPT_PCM_BYTES)/2*3;
        unsigned left=end-ui_feedback.offset,count=left>DECODE_FRAMES?DECODE_FRAMES:left;
        int16_t silence[DECODE_FRAMES]={0};if(play_samples(a,silence,(int)count,deadline))return -1;feedback_tail+=count;
    }
    if(feedback_tail) {
        snd_pcm_uframes_t buffer=0,period=0;
        if(snd_pcm_get_params(a->pcm,&buffer,&period)<0||!period||period>buffer)return fail("feedback playback period unavailable");
        unsigned left=(unsigned)((period-(a->samples+feedback_tail)%period)%period);
        a->ui_prompt=1;
        while(left) {
            unsigned count=left>DECODE_FRAMES?DECODE_FRAMES:left;int16_t silence[DECODE_FRAMES]={0};
            if(play_samples(a,silence,(int)count,deadline)){a->ui_prompt=0;return -1;}left-=count;
        }
        a->ui_prompt=0;
    }
    for(;;) {
        panel_poll();if(stopping||panel_interrupted)return -1;
        int state=snd_pcm_state(a->pcm);
        if(state==S12_STATE_XRUN)return fail("ALSA playback XRUN");
        if(state==S12_STATE_SUSPENDED||state==S12_STATE_DISCONNECTED)return fail("ALSA incomplete playback");
        if(state==S12_STATE_SETUP)return 0;
        int r=snd_pcm_drain(a->pcm);if(r==0)return 0;
        if(r==-EPIPE)return fail("ALSA playback XRUN");
        if(r!=-EAGAIN||!remaining(deadline))return fail("ALSA drain failed or timed out");
        /* A draining handle can report ready continuously; avoid a busy loop. */
        struct timespec delay={0,5000000};nanosleep(&delay,NULL);
    }
}
#endif
static int decode_audio(struct audio *a,const uint8_t *data,size_t len,int64_t deadline) {
    if(!len||len>MAX_OPUS||++a->packets>(a->music?MAX_MUSIC_AUDIO_PACKETS:MAX_AUDIO_PACKETS))return fail("Opus packet limit exceeded");
    if(!a->decoder) {
        int err=0;
        /* Opus performs the output resampling internally. Every negotiated
         * Opus sample rate can be decoded directly to physical 48 kHz. */
        a->decoder=opus_decoder_create(48000,1,&err);
        if(!a->decoder||err)return fail("Opus decoder creation failed");
    }
    int16_t pcm[DECODE_FRAMES];
    int n=opus_decode(a->decoder,data,(int)len,pcm,DECODE_FRAMES,0);
    if(n<=0||n>DECODE_FRAMES)return fail("Opus packet decode failed");
    a->samples+=(unsigned)n;
#ifndef XIAOZHI_TEST_NO_ALSA
    if(a->playback){if(play_samples(a,pcm,n,deadline))return -1;++a->played_packets;}
#else
    (void)deadline;
    if(a->playback) {
        return fail("test binary has no ALSA playback");
    }
#endif
    return 0;
}
static void close_audio(struct audio *a) {
    if(!a->ui_prompt)ui_feedback.active=0;
#ifndef XIAOZHI_TEST_NO_ALSA
    if(a->pcm) {snd_pcm_drop(a->pcm);snd_pcm_close(a->pcm);a->pcm=NULL;}
#endif
    if(a->decoder){opus_decoder_destroy(a->decoder);a->decoder=NULL;}
}
static unsigned farewell_last=S12_FAREWELL_PROMPT_COUNT;
static unsigned farewell_pick(void) {
    uint32_t random=0;unsigned count=S12_FAREWELL_PROMPT_COUNT;
    if(count<=1){farewell_last=0;return 0;}
    if(random_bytes((uint8_t *)&random,sizeof(random)))random=(uint32_t)now_ms();
    unsigned choices=farewell_last<count?count-1:count;
    unsigned selected=random%choices;
    if(farewell_last<count&&selected>=farewell_last)++selected;
    farewell_last=selected;return selected;
}
static int local_prompt(const char *name,const struct audio *options) {
    const char *path=NULL;const uint8_t *embedded=NULL;size_t prompt_bytes=0;
    if(!strcmp(name,"wake"))path="/s12/wakeup.pcm";
    else if(!strcmp(name,"boot"))path="/s12/bootup.pcm";
    else if(!strcmp(name,"wifi"))path="/s12/wifi-unavailable.pcm";
    else if(!strcmp(name,"cloud"))path="/s12/cloud-unavailable.pcm";
    else if(!strcmp(name,"followup")){embedded=s12_followup_prompt_pcm;prompt_bytes=S12_FOLLOWUP_PROMPT_PCM_BYTES;}
    else if(!strcmp(name,"volume")){embedded=s12_volume_prompt_pcm;prompt_bytes=S12_VOLUME_PROMPT_PCM_BYTES;}
    else if(!strcmp(name,"button")||!strcmp(name,"command-key")){embedded=s12_button_prompt_pcm;prompt_bytes=S12_BUTTON_PROMPT_PCM_BYTES;}
    else if(!strcmp(name,"farewell")) {
        unsigned selected=farewell_pick();embedded=s12_farewell_clips[selected].pcm;prompt_bytes=s12_farewell_clips[selected].bytes;
        printf("{\"event\":\"farewell_selected\",\"clip\":%u,\"network_required\":false}\n",selected);
    }
    else return -1;
    printf("{\"event\":\"local_prompt\",\"prompt\":\"%s\",\"network_required\":false}\n",name);
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    (void)path;(void)options;(void)embedded;(void)prompt_bytes;
    const char *test_delay=getenv("S12_TEST_UI_PROMPT_MS");
    if(test_delay&&(!strcmp(name,"volume")||!strcmp(name,"button")||!strcmp(name,"followup")||!strcmp(name,"farewell")||!strcmp(name,"command-key"))) {
        unsigned ms=(unsigned)strtoul(test_delay,NULL,10);if(ms>500)ms=500;int64_t end=now_ms()+ms;
        while(remaining(end)&&!stopping&&!panel_interrupted){panel_poll();struct timespec pause={0,5000000};nanosleep(&pause,NULL);}
    }
    return stopping||panel_interrupted?-1:0;
#else
    int fd=-1;
    if(!embedded) {
        fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return fail("stock local prompt unavailable");
        struct stat st;
        if(metadata_stat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>160000||(st.st_size&1)||
           (!strcmp(name,"boot")&&(st.st_uid!=geteuid()||st.st_nlink!=1||(st.st_mode&0777)!=0600))){
            close(fd);return fail("stock local prompt format invalid");
        }
        prompt_bytes=(size_t)st.st_size;
    }
    if(!prompt_bytes||prompt_bytes>160000||(prompt_bytes&1)){if(fd>=0)close(fd);return fail("stock local prompt format invalid");}
    struct audio a;memset(&a,0,sizeof(a));a.gain=options->gain;a.ui_prompt=1;a.ready_prompt=!strcmp(name,"followup");
    int rc=-1;int64_t deadline=now_ms()+8000;size_t total=0;
    int16_t mono[1920],resampled[5760];
    while(!stopping&&total<prompt_bytes) {
        size_t count=prompt_bytes-total;if(count>sizeof(mono))count=sizeof(mono);
        size_t at=0;
        if(embedded){memcpy(mono,embedded+total,count);at=count;}
        else while(at<count){ssize_t n=read(fd,(char *)mono+at,count-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;}
        total+=at;
        for(unsigned i=0;i<at/2;++i)for(unsigned phase=0;phase<3;++phase)resampled[i*3+phase]=mono[i];
        if(play_samples(&a,resampled,(int)(at/2*3),deadline))goto done;
    }
    /* Match the original player's complete-period EOF submission. On this
     * board a partial last period can audibly replay part of a short prompt
     * despite a successful drain. Align startup and all embedded local cues; preserve
     * every original sample, the nonblocking drain and immediate capture
     * resume. This is not a fixed leading/trailing timing delay. */
    if(!strcmp(name,"wake")||!strcmp(name,"boot")||embedded) {
        snd_pcm_uframes_t buffer=0,period=0;
        if(stopping||panel_interrupted)goto done;
        if(!a.pcm||snd_pcm_get_params(a.pcm,&buffer,&period)<0||!period||period>buffer) {
            fail("stock local playback period unavailable");goto done;
        }
        uint64_t frames=total/2*3;
        snd_pcm_uframes_t left=(period-(frames%period))%period;
        memset(resampled,0,sizeof(resampled));
        while(left&&!stopping) {
            unsigned count=left>5760?5760:(unsigned)left;
            if(play_samples(&a,resampled,(int)count,deadline))goto done;
            left-=count;
        }
    }
    if(!stopping&&!drain_playback(&a,deadline))rc=0;
done:
    if(fd>=0)close(fd);close_audio(&a);wipe(mono,sizeof(mono));wipe(resampled,sizeof(resampled));return rc;
#endif
}
/* Called only with every capture reader/DSP worker joined and closed. A
 * microphone mute changes input privacy, not explicit button confirmation.
 * 0 means no cue claimed, 1 completed, -1 failed/cancelled. */
static int ui_feedback_play(const struct audio *options) {
    if(stopping||ota_idle_requested()||!ui_feedback_ready())return 0;
    if(panel_enabled&&!panel.gain){ui_feedback.pending=ui_feedback.active=0;return 0;}
    unsigned kind=ui_feedback.pending;ui_feedback.pending=0;ui_feedback.last=now_ms();
    int64_t queued=ui_feedback.last-ui_feedback.first;if(queued<0)queued=0;if(queued>600000)queued=600000;
    printf("{\"event\":\"ui_feedback\",\"mode\":\"capture_closed\",\"queued_ms\":%lld,\"microphone_uploaded\":false}\n",(long long)queued);
    return local_prompt(kind==1?"volume":"button",options)?-1:1;
}
static int local_network_ready(void) {
    char path[4096],text[16];if(panel_path(path,sizeof(path),"network.state"))return 0;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return 0;
    ssize_t n=read(fd,text,sizeof(text));close(fd);return n==6&&!memcmp(text,"ready\n",6);
}
static void mic_frontend_report(const struct mic_frontend *m,const char *phase) {
    if(m->mode!=MIC_AUTO)return;
    printf("{\"event\":\"mic_array\",\"phase\":\"%s\",\"selected\":%u,\"locked\":%s,\"channels\":[",
           phase,m->selected,m->locked?"true":"false");
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)
        printf("%s{\"channel\":%u,\"healthy\":%s,\"rms\":%.1f,\"noise_rms\":%.1f,\"level_over_floor\":%.2f,\"clip_fraction\":%.4f}",
               ch?",":"",ch,m->healthy[ch]?"true":"false",m->rms[ch],m->noise[ch],m->score[ch],m->clip[ch]);
    puts("]}");
}
struct capture {
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS
    uint64_t debug_id;
#endif
    uint64_t frames;
    unsigned short_reads;
    int64_t deadline;
    pthread_t reader;
    pthread_mutex_t mutex;
    pthread_cond_t available;
    int mutex_ready,condition_ready,reader_started,reader_stop,reader_error,reported,finished,finish_result;
    int reader_prompt_drop,reader_prompt_quiesced,reader_prompt_ready,reader_prompt_resume;uint64_t prompt_discarded_frames;
    int32_t *fifo;
    unsigned fifo_read,fifo_write,fifo_used,fifo_peak;
    uint64_t source_frames,delivered_frames;
#ifndef XIAOZHI_TEST_NO_ALSA
    snd_pcm_t *pcm;
#endif
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    int64_t started;
    unsigned fake_calls;
    unsigned fake_generation;
    uint64_t fake_time_origin;
    int fake_command,fake_ready_requested,fake_ready_phase,fake_boundary_wait;
    unsigned fake_boundary_retained;uint64_t fake_boundary_first_frame;
#endif
};
/* Linux ALSA reports a suspended capture as ESTRPIPE. Darwin's native mock
 * build has no such errno; retain the target value for that fixture only. */
#ifndef ESTRPIPE
#define ESTRPIPE 86
#endif
#define CAPTURE_DISCONTINUITY (-2)
#define LOCAL_CAPTURE_RETRY 5
#define LOCAL_STARTUP_PROBE 6
#define FOLLOWUP_SESSION_CLOSED 7
#define LOCAL_UI_FEEDBACK 9
/* Three seconds is a fixed peak-work allowance, not a prefill or a latency
 * target. Overflow still invalidates the whole turn; no samples are skipped. */
#define CAPTURE_FIFO_FRAMES 144000u
#define CAPTURE_READER_FRAMES 1024u
#define CAPTURE_FIFO_OVERFLOW (-4096)
static void *capture_reader(void *opaque);
static int capture_finish(struct capture *c);
static int capture_discontinuity(long error,const char *stage) {
    if(error!=-EPIPE&&error!=-ESTRPIPE&&error!=CAPTURE_FIFO_OVERFLOW)return 0;
    if(!barge_in_worker()){barge_failure="capture";barge_error_reason="capture_discontinuity";}
    const char *cause=error==-EPIPE?"xrun":error==-ESTRPIPE?"suspended":"fifo_overflow";
    fprintf(stderr,"xiaozhi: ALSA capture %s; damaged audio discarded\n",error==-EPIPE?"XRUN":cause);
    printf("{\"event\":\"capture_discontinuity\",\"stage\":\"%s\",\"cause\":\"%s\",\"audio_uploaded\":false}\n",
           stage,cause);
    return CAPTURE_DISCONTINUITY;
}
static void capture_close(struct capture *c) {
    (void)capture_finish(c);
}
static int capture_open(struct capture *c) {
#ifndef XIAOZHI_TEST_NO_ALSA
    const char *name=getenv("S12_CAPTURE_PCM");if(!name)name="hw:0,2";
    if(strcmp(name,"hw:0,2")&&strcmp(name,"s12cap"))return fail("capture PCM must be hw:0,2 or s12cap");
    if(snd_pcm_open(&c->pcm,name,S12_CAPTURE,S12_NONBLOCK)<0)return fail("ALSA capture open failed");
    snd_pcm_hw_params_t *h=NULL;int rc=-1,dir=0,format=-1;unsigned ch=0,rate=0;
    snd_pcm_uframes_t period=960,buffer=15360,format_limit=0,period_limit=0,actual_period=0,actual_buffer=0;
    if(snd_pcm_hw_params_malloc(&h)<0||snd_pcm_hw_params_any(c->pcm,h)<0||
       snd_pcm_hw_params_set_access(c->pcm,h,S12_RW_INTERLEAVED)<0||
       snd_pcm_hw_params_set_format(c->pcm,h,S12_S32_LE)<0||
       snd_pcm_hw_params_set_channels(c->pcm,h,CAPTURE_CHANNELS)<0||
       snd_pcm_hw_params_set_rate(c->pcm,h,48000,0)<0||
       snd_pcm_hw_params_get_buffer_size_max(h,&format_limit)<0||
       snd_pcm_hw_params_set_period_size_near(c->pcm,h,&period,&dir)<0||
       snd_pcm_hw_params_get_buffer_size_max(h,&period_limit)<0||
       snd_pcm_hw_params_set_buffer_size_near(c->pcm,h,&buffer)<0||
       snd_pcm_hw_params(c->pcm,h)<0||
       snd_pcm_hw_params_get_format(h,&format)<0||snd_pcm_hw_params_get_channels(h,&ch)<0||
       snd_pcm_hw_params_get_rate(h,&rate,&dir)<0||format!=S12_S32_LE||ch!=CAPTURE_CHANNELS||rate!=48000||
       snd_pcm_get_params(c->pcm,&actual_buffer,&actual_period)<0||!actual_period||actual_buffer<actual_period||
       snd_pcm_prepare(c->pcm)<0)fail("ALSA exact capture setup failed");
    else {
        printf("{\"event\":\"capture_hardware\",\"rate\":48000,\"channels\":8,\"period_frames\":%lu,\"buffer_frames\":%lu,\"format_limit_buffer_frames\":%lu,\"period_limit_buffer_frames\":%lu,\"fifo_frames\":%u}\n",
               actual_period,actual_buffer,format_limit,period_limit,CAPTURE_FIFO_FRAMES);rc=0;
    }
    if(h)snd_pcm_hw_params_free(h);
    return rc;
#elif defined(XIAOZHI_TEST_FAKE_CAPTURE)
    (void)c;return 0;
#else
    (void)c;return fail("test binary has no capture source");
#endif
}
static int capture_start(struct capture *c) {
    c->fifo=malloc((size_t)CAPTURE_FIFO_FRAMES*CAPTURE_CHANNELS*sizeof(int32_t));
    if(!c->fifo)return fail("capture FIFO allocation failed");
    if(pthread_mutex_init(&c->mutex,NULL))return fail("capture mutex initialization failed");
    c->mutex_ready=1;
    pthread_condattr_t attributes;
    if(pthread_condattr_init(&attributes))return fail("capture condition attributes failed");
#if !defined(__APPLE__)
    if(pthread_condattr_setclock(&attributes,CLOCK_MONOTONIC)){pthread_condattr_destroy(&attributes);return fail("capture monotonic wait unavailable");}
#endif
    int condition=pthread_cond_init(&c->available,&attributes);pthread_condattr_destroy(&attributes);
    if(condition)return fail("capture condition initialization failed");c->condition_ready=1;
#ifndef XIAOZHI_TEST_NO_ALSA
    int r=snd_pcm_start(c->pcm);
    if(capture_discontinuity(r,"start"))return CAPTURE_DISCONTINUITY;
    if(r<0)return fail("ALSA capture start failed");
#elif defined(XIAOZHI_TEST_FAKE_CAPTURE)
    static unsigned generations;
    c->started=now_ms();c->fake_generation=generations++;
#else
    return -1;
#endif
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS
    c->debug_id=s12dbg_capture_start();
#endif
    if(pthread_create(&c->reader,NULL,capture_reader,c))return fail("capture reader thread creation failed");
    c->reader_started=1;
    printf("{\"event\":\"capture_reader_started\",\"fifo_frames\":%u,\"reader_frames\":%u,\"audio_saved\":false}\n",CAPTURE_FIFO_FRAMES,CAPTURE_READER_FRAMES);
    return 0;
}
static long capture_source_read(struct capture *c,int32_t *buffer,unsigned frames) {
#ifndef XIAOZHI_TEST_NO_ALSA
    int state=snd_pcm_state(c->pcm);
    if(state==S12_STATE_XRUN)return -EPIPE;
    if(state==S12_STATE_SUSPENDED)return -ESTRPIPE;
    if(state==S12_STATE_DISCONNECTED)return -EIO;
    return snd_pcm_readi(c->pcm,buffer,frames);
#elif defined(XIAOZHI_TEST_FAKE_CAPTURE)
    /* This source exists ONLY in the native offline-test binary. It supplies
     * synthetic 8-channel samples in RAM with legal short reads and real-time
     * pacing. The production ARM build has no fake-source environment hooks. */
    const char *fault=getenv("S12_TEST_CAPTURE_FAULT");
    pthread_mutex_lock(&c->mutex);
    /* The boundary fixture blocks a real in-flight tiny read until the main
     * owner reaches cue EOF. Its source epoch is set there, never at ack. */
    if(c->fake_ready_requested&&c->reader_prompt_drop&&!c->fake_ready_phase&&getenv("S12_TEST_READY_FORCE_BOUNDARY")) {
        c->fake_boundary_wait=1;pthread_cond_broadcast(&c->available);
        while(!c->fake_ready_phase&&!c->reader_stop)pthread_cond_wait(&c->available,&c->mutex);
    }
    uint64_t delivered=c->delivered_frames,origin=c->fake_time_origin;int ready_phase=c->fake_ready_phase;
    pthread_mutex_unlock(&c->mutex);
    if(fault&&!strcmp(fault,"xrun")&&c->source_frames>=CAPTURE_CHUNK)return -EPIPE;
    if(fault&&!strcmp(fault,"timeout"))return -EAGAIN;
    if(fault&&!strcmp(fault,"io"))return -EIO;
    if(fault&&!strcmp(fault,"startup-resume-xrun-once")&&c->fake_generation==1)return -EPIPE;
    if(fault&&c->source_frames>=CAPTURE_CHUNK) {
        if(!strcmp(fault,"idle-xrun-once")&&c->fake_generation==0)return -EPIPE;
        if(!strcmp(fault,"idle-suspend-once")&&c->fake_generation==0)return -ESTRPIPE;
        if(!strcmp(fault,"command-xrun-once")&&c->fake_command&&c->fake_generation==1&&delivered>=CAPTURE_CHUNK*10)return -EPIPE;
        if(!strcmp(fault,"sentence-xrun-once")&&c->fake_generation==0&&delivered>=CAPTURE_CHUNK*19)return -EPIPE;
        if(!strcmp(fault,"xrun-two-then-stable")&&(c->fake_generation<2||
           (c->fake_generation==2&&delivered>=CAPTURE_CHUNK*85)))return -EPIPE;
        if(!strcmp(fault,"xrun-repeat"))return -EPIPE;
    }
    if(++c->fake_calls%3==1&&frames>1)frames/=2;
    const char *profile=getenv("S12_TEST_CAPTURE_PROFILE");
    const char *silent_from=getenv("S12_TEST_CAPTURE_SILENT_FROM_GENERATION");
    if(silent_from&&c->fake_generation>=(unsigned)strtoul(silent_from,NULL,10))profile="wake-silence";
    const char *fast=getenv("S12_TEST_CAPTURE_FAST");
    unsigned speed=fast&&!strcmp(fast,"1")?100:1;
    int64_t due=c->started+(int64_t)((c->source_frames-origin+frames)*1000/(48000*speed));
    if(now_ms()<due)return -EAGAIN;
    for(unsigned i=0;i<frames;++i) {
        double t=(double)(c->source_frames-origin+i)/48000.0;
        for(unsigned ch=0;ch<CAPTURE_CHANNELS;++ch) {
            double frequency=ch==0?1000:ch==1?12000:500+100*ch;
            double signal=536870912.0*sin(2*PI*frequency*t);
            if(profile) {
                double end=!strcmp(profile,"wake-long")?5.5:!strcmp(profile,"wake-max")?20.0:0.9;
                int voiced=strcmp(profile,"wake-silence")&&t>=0.3&&t<end;
                const char *immediate=getenv("S12_TEST_COMMAND_IMMEDIATE");
                if(c->fake_command&&immediate&&!strcmp(immediate,"1"))voiced=t<end;
                const char *continuation=getenv("S12_TEST_LOCAL_CONTINUATION");
                if(local_wake_enabled&&!c->fake_command&&getenv("S12_TEST_LOCAL_WAKE_FRAME")&&!continuation)voiced=0;
                /* Constant offset must not trigger de-meaned RMS VAD. Channel
                 * one also proves the anti-alias filter rejects 12 kHz. */
                if(!strcmp(profile,"wake-soft")||!strcmp(profile,"wake-soft-noise")) {
                    /* ~45 S16 RMS speech plus ~6 RMS noise and a 4000-unit
                     * DC offset matches the measured quiet S12 mic scale. */
                    if(!strcmp(profile,"wake-soft-noise"))voiced=0;
                    signal=262144000.0+524288.0*sin(2*PI*300*t)+(voiced?signal/128.0:0);
                } else signal=262144000.0+(voiced?signal:0);
                if(!strcmp(profile,"wake-array")||!strcmp(profile,"local-sentence")||!strcmp(profile,"wake-cue-trace")||!strcmp(profile,"wake-weak-beam")) {
                    /* Stationary loud interference, a dead mic, two opposite
                     * clean speech phases, a weak mic and a clipping mic. */
                    double noise=ch==0?8000:ch==1?0:8;
                    double speech=ch==2?700:ch==4?-700:ch==3?15:ch==5?60000:0;
                    if(!strcmp(profile,"wake-weak-beam")){noise=8;speech=8;}
                    double speech_hz=c->fake_command&&immediate&&!strcmp(immediate,"1")&&t<.06?443:997;
                    if(!strcmp(profile,"local-sentence")) {
                        const char *b=getenv("S12_TEST_SENTENCE_BEGIN_MS"),*e=getenv("S12_TEST_SENTENCE_END_MS");
                        const char *cs=getenv("S12_TEST_CONTINUATION_START_MS"),*ce=getenv("S12_TEST_CONTINUATION_END_MS");
                        double begin=b?strtod(b,NULL)/1000:.3,finish=e?strtod(e,NULL)/1000:.5;
                        double command_start=cs?strtod(cs,NULL)/1000:finish,command_end=ce?strtod(ce,NULL)/1000:command_start+.3;
                        int keyword=t>=begin&&t<finish,command=t>=command_start&&t<command_end;
                        /* First wake is standalone, its fresh command window
                         * is silent, and the next capture supplies a sentence.
                         * This models a real eight-second timeout without
                         * resetting either persistent fixture KWS clock. */
                        if(getenv("S12_TEST_CONTINUATION_AFTER_TIMEOUT")&&c->fake_generation==0)command=0;
                        voiced=!c->fake_command&&(keyword||command);
                        speech_hz=keyword?443:997;
                        /* A separate idle sentence must not enter the
                         * authorized keyword/continuation upload interval. */
                        if(!c->fake_command&&t>=.12&&t<.18){voiced=1;speech_hz=1301;}
                    }
                    double ready_echo_sample=0,ready_user_harmonic=0;
                    const char *short_ms=getenv("S12_TEST_READY_SHORT_MS"),*echo_ms=getenv("S12_TEST_READY_ECHO_MS");
                    if(c->fake_ready_requested&&(short_ms||echo_ms)) {
                        const char *offset=getenv("S12_TEST_READY_ONSET_MS");double onset=offset?strtod(offset,NULL)/1000:0;
                        double duration=short_ms?strtod(short_ms,NULL)/1000:0;
                        voiced=ready_phase&&t>=onset&&t<onset+duration;
                        const char *user_hz=getenv("S12_TEST_READY_USER_HZ"),*user_harmonic=getenv("S12_TEST_READY_USER_HARMONIC"),*user_decay=getenv("S12_TEST_READY_USER_DECAY_MS");
                        speech_hz=t<onset+.06?(user_hz?strtod(user_hz,NULL):443):997;
                        if(user_hz&&user_harmonic){speech_hz=strtod(user_hz,NULL);ready_user_harmonic=voiced?speech*strtod(user_harmonic,NULL)*sin(2*PI*speech_hz*2*t):0;}
                        if(user_decay&&voiced){double decay=exp(-6.90775527898*(t-onset)/(strtod(user_decay,NULL)/1000));speech*=decay;ready_user_harmonic*=decay;}
                        if(echo_ms&&ready_phase&&t<strtod(echo_ms,NULL)/1000) {
                            const char *rms=getenv("S12_TEST_READY_ECHO_RMS");double level=rms?strtod(rms,NULL):700;
                            const char *hz=getenv("S12_TEST_READY_ECHO_HZ");double frequency=hz?strtod(hz,NULL):550;
                            ready_echo_sample=level*1.41421356237*exp(-6.90775527898*t/(strtod(echo_ms,NULL)/1000))*sin(2*PI*frequency*t);
                        }
                    }
                    signal=65536.0*((ch==1?0:4000)+noise*sin(2*PI*(211+31*ch)*(c->fake_ready_requested?(double)(c->source_frames+i)/48000:t))+
                        (voiced?speech*sin(2*PI*speech_hz*t):0)+ready_echo_sample+ready_user_harmonic);
                    if(!strcmp(profile,"wake-cue-trace")) {
                        const double trace[]={500,80,40,28,18,16,12,9,7,7,7,7,7,7,7,7,7,7,7,7};
                        unsigned block=(unsigned)(t/.06);double rms=c->fake_command?trace[block<20?block:19]:7;
                        signal=65536.0*(4000+rms*1.41421356237*sin(2*PI*(211+31*ch)*t));
                    }
#ifdef S12_TEST_READY_PCM_SAMPLE
                    /* Native harness injection of real short human speech;
                     * absent from every shipping build and normal fixture. */
                    double fixture=S12_TEST_READY_PCM_SAMPLE(t,ch);
                    if(c->fake_ready_requested&&!isnan(fixture))signal=65536.0*((ch==1?0:4000)+
                        noise*sin(2*PI*(211+31*ch)*(double)(c->source_frames+i)/48000)+(ready_phase?fixture:0));
#endif
                    if(c->fake_ready_requested&&ch==5)signal=INT32_MAX; /* Stable clipped fixture microphone. */
                    if(ch>=6)signal=1400000000.0*sin(2*PI*997*t);
                    if(signal>INT32_MAX)signal=INT32_MAX;if(signal<INT32_MIN)signal=INT32_MIN;
                }
            }
            buffer[i*CAPTURE_CHANNELS+ch]=(int32_t)signal;
        }
    }
    return (long)frames;
#else
    (void)c;(void)buffer;(void)frames;return -EIO;
#endif
}
/* The producer alone reads/waits on the nonblocking PCM. It performs no
 * model, UI, file-log or network work. A discontinuity is latched before any
 * further consumer read; old audio is never overwritten to hide a gap. */
static void capture_reader_error(struct capture *c,int error) {
    pthread_mutex_lock(&c->mutex);if(!c->reader_error)c->reader_error=error;
    pthread_cond_broadcast(&c->available);pthread_mutex_unlock(&c->mutex);
}
static void *capture_reader(void *opaque) {
    struct capture *c=opaque;int32_t raw[CAPTURE_READER_FRAMES*CAPTURE_CHANNELS];
    for(;;) {
        pthread_mutex_lock(&c->mutex);int stop=c->reader_stop;s12dbg_reader(c->debug_id,c->source_frames,c->fifo_used,c->reader_error);pthread_mutex_unlock(&c->mutex);if(stop)break;
        pthread_mutex_lock(&c->mutex);
        int prompt_drop=c->reader_prompt_drop;
        if(prompt_drop&&!c->reader_prompt_quiesced){c->reader_prompt_quiesced=1;pthread_cond_broadcast(&c->available);}
        if(!prompt_drop&&!c->reader_prompt_ready) {
            c->reader_prompt_resume=0;c->reader_prompt_ready=1;pthread_cond_broadcast(&c->available);
        }
        pthread_mutex_unlock(&c->mutex);
        /* During a cue, small reads bound the final in-flight discarded block
         * to one millisecond. The reader never stops/reopens at cue EOF. */
        unsigned requested=prompt_drop?48u:CAPTURE_READER_FRAMES;
        long count=capture_source_read(c,raw,requested);
        if(count==-EINTR)continue;
        if(!count||count==-EAGAIN) {
#ifndef XIAOZHI_TEST_NO_ALSA
            int waited=snd_pcm_wait(c->pcm,20);if(waited<0&&waited!=-EINTR){capture_reader_error(c,waited);break;}
#else
            struct timespec pause={0,1000000};nanosleep(&pause,NULL);
#endif
            continue;
        }
        if(count<0||(unsigned long)count>CAPTURE_READER_FRAMES){capture_reader_error(c,count<0?(int)count:-EIO);break;}
        pthread_mutex_lock(&c->mutex);
        c->source_frames+=(unsigned)count;
        if(c->reader_stop){pthread_mutex_unlock(&c->mutex);break;}
        if(c->reader_prompt_drop) {
            c->delivered_frames+=(unsigned)count;c->prompt_discarded_frames+=(unsigned)count;
            pthread_mutex_unlock(&c->mutex);continue;
        }
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
        if(prompt_drop&&c->fake_boundary_wait) {
            c->fake_boundary_retained+=(unsigned)count;c->fake_boundary_first_frame=c->source_frames-count-c->fake_time_origin;
        }
#endif
        /* At EOF keep the final in-flight tiny block, even if its read began
         * during the cue. At most1ms of cue tail is safer than losing onset. */
        if((unsigned)count>CAPTURE_FIFO_FRAMES-c->fifo_used) {
            c->reader_error=CAPTURE_FIFO_OVERFLOW;pthread_cond_broadcast(&c->available);pthread_mutex_unlock(&c->mutex);break;
        }
        unsigned first=CAPTURE_FIFO_FRAMES-c->fifo_write;if(first>(unsigned)count)first=(unsigned)count;
        memcpy(c->fifo+(size_t)c->fifo_write*CAPTURE_CHANNELS,raw,(size_t)first*CAPTURE_CHANNELS*sizeof(int32_t));
        if(first<(unsigned)count)memcpy(c->fifo,raw+(size_t)first*CAPTURE_CHANNELS,((unsigned)count-first)*CAPTURE_CHANNELS*sizeof(int32_t));
        c->fifo_write=(c->fifo_write+(unsigned)count)%CAPTURE_FIFO_FRAMES;c->fifo_used+=(unsigned)count;
        if(c->fifo_used>c->fifo_peak)c->fifo_peak=c->fifo_used;
        pthread_cond_signal(&c->available);pthread_mutex_unlock(&c->mutex);
    }
    wipe(raw,sizeof(raw));return NULL;
}
static long capture_read(struct capture *c,int32_t *buffer,unsigned frames) {
    pthread_mutex_lock(&c->mutex);
    if(c->reader_error){int error=c->reader_error;pthread_mutex_unlock(&c->mutex);return error;}
    unsigned count=c->fifo_used;if(count>frames)count=frames;
    unsigned first=CAPTURE_FIFO_FRAMES-c->fifo_read;if(first>count)first=count;
    if(count) {
        memcpy(buffer,c->fifo+(size_t)c->fifo_read*CAPTURE_CHANNELS,(size_t)first*CAPTURE_CHANNELS*sizeof(int32_t));
        if(first<count)memcpy(buffer+(size_t)first*CAPTURE_CHANNELS,c->fifo,(size_t)(count-first)*CAPTURE_CHANNELS*sizeof(int32_t));
        c->fifo_read=(c->fifo_read+count)%CAPTURE_FIFO_FRAMES;c->fifo_used-=count;
        c->delivered_frames+=count;
    }
    pthread_mutex_unlock(&c->mutex);return count?(long)count:-EAGAIN;
}
static void capture_wait(struct capture *c) {
    pthread_mutex_lock(&c->mutex);
    if(!c->fifo_used&&!c->reader_error&&!c->reader_stop) {
        struct timespec until;
#if defined(__APPLE__)
        until.tv_sec=0;until.tv_nsec=20000000;
        (void)pthread_cond_timedwait_relative_np(&c->available,&c->mutex,&until);
#else
        clock_gettime(CLOCK_MONOTONIC,&until);until.tv_nsec+=20000000;
        if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
        (void)pthread_cond_timedwait(&c->available,&c->mutex,&until);
#endif
    }
    pthread_mutex_unlock(&c->mutex);
}
/* Successful final audio is not announced until this joins the reader and
 * checks its last error plus hardware state. Drop/close are never concurrent
 * with readi/wait; no DMA handle survives this finish, mute, OTA ack or retry.
 * The explicit ready cue keeps this one reader alive until its own finish. */
static int capture_finish(struct capture *c) {
    if(c->finished)return c->finish_result;c->finished=1;
    int had_reader=c->reader_started;
    if(c->reader_started) {
        pthread_mutex_lock(&c->mutex);c->reader_stop=1;pthread_cond_broadcast(&c->available);pthread_mutex_unlock(&c->mutex);
        if(pthread_join(c->reader,NULL)){(void)fail("capture reader join failed; terminating before releasing shared state");_exit(1);}c->reader_started=0;
    }
    s12dbg_reader(c->debug_id,c->source_frames,c->fifo_used,c->reader_error);s12dbg_capture_close(c->debug_id);
    int error=c->reader_error;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    if(getenv("S12_TEST_CAPTURE_LATE_ERROR")&&c->fake_generation==0&&c->frames>=48000&&!stopping)error=-EPIPE;
    const char *test_fault=getenv("S12_TEST_CAPTURE_FAULT");
    if(test_fault&&!strcmp(test_fault,"startup-close-xrun-once")&&c->fake_generation==0&&c->frames>=CAPTURE_CHUNK&&!stopping)error=-EPIPE;
#endif
#ifndef XIAOZHI_TEST_NO_ALSA
    if(c->pcm) {
        int state=snd_pcm_state(c->pcm);
        if(!error&&state==S12_STATE_XRUN)error=-EPIPE;
        if(!error&&state==S12_STATE_SUSPENDED)error=-ESTRPIPE;
        if(!error&&state==S12_STATE_DISCONNECTED)error=-EIO;
        (void)snd_pcm_drop(c->pcm);snd_pcm_close(c->pcm);c->pcm=NULL;
    }
#endif
    if(c->condition_ready){pthread_cond_destroy(&c->available);c->condition_ready=0;}
    if(c->mutex_ready){pthread_mutex_destroy(&c->mutex);c->mutex_ready=0;}
    if(c->fifo) {
        printf("{\"event\":\"capture_reader_closed\",\"reader_joined\":%s,\"consumed_frames\":%llu,\"source_frames\":%llu,\"fifo_peak_frames\":%u,\"queued_frames_discarded\":%u}\n",
               had_reader?"true":"false",(unsigned long long)c->frames,(unsigned long long)c->source_frames,c->fifo_peak,c->fifo_used);
        wipe(c->fifo,(size_t)CAPTURE_FIFO_FRAMES*CAPTURE_CHANNELS*sizeof(int32_t));free(c->fifo);c->fifo=NULL;
    }
    if(error) {
        if(c->reported)c->finish_result=(error==-EPIPE||error==-ESTRPIPE||error==CAPTURE_FIFO_OVERFLOW)?CAPTURE_DISCONTINUITY:-1;
        else {c->finish_result=capture_discontinuity(error,"reader_close");if(!c->finish_result)c->finish_result=fail("ALSA capture reader failed");c->reported=1;}
    }
    return c->finish_result;
}
static int capture_exact(struct capture *c,int32_t *buffer,unsigned frames) {
    unsigned off=0;
    while(off<frames) {
        panel_poll();if(panel_interrupted)return -1;
        if(followup_capture_deadline&&session_keepalive.failed)return FOLLOWUP_SESSION_CLOSED;
        if(stopping)return fail("interrupted");
        if(!remaining(c->deadline))return fail("capture deadline expired with incomplete audio");
        long n=capture_read(c,buffer+off*CAPTURE_CHANNELS,frames-off);
        if(n==-EINTR)continue;
        if(n==0||n==-EAGAIN) {
            capture_wait(c);
            continue;
        }
        if(capture_discontinuity(n,"reader")){c->reported=1;return CAPTURE_DISCONTINUITY;}
        if(n<0||(unsigned long)n>frames-off){c->reported=1;return fail("ALSA capture transfer failed");}
        if((unsigned long)n<frames-off)++c->short_reads;
        off+=(unsigned)n;c->frames+=(unsigned)n;s12dbg_consumed(c->debug_id,c->frames);
    }
    return 0;
}
static int send_listen(struct wire *w,const char *sid,int start) {
    if(start&&dialogue_begin(w))return -1;
    struct json_object *j=json_object_new_object();if(!j)return fail("JSON allocation failed");
    add_string(j,"session_id",sid);add_string(j,"type","listen");add_string(j,"state",start?"start":"stop");
    dialogue_add_id(w,j);
    if(start)add_string(j,"mode","manual");
    return send_json(w,j);
}
static int capture_upload(struct wire *w,const char *sid,unsigned channel,unsigned seconds) {
    struct capture c;memset(&c,0,sizeof(c));
    struct mic_frontend frontend;mic_frontend_init(&frontend,channel);
    int err=0,rc=-1;unsigned packets=0,padding=0,onset=0;
    OpusEncoder *encoder=opus_encoder_create(16000,1,2048,&err); /* OPUS_APPLICATION_VOIP */
    if(!encoder||err){if(encoder)opus_encoder_destroy(encoder);return fail("Opus encoder creation failed");}
    /* Public OPUS_SET_BITRATE(24000), OPUS_SET_COMPLEXITY(4), ABI since 1.0. */
    if(opus_encoder_ctl(encoder,4002,24000)||opus_encoder_ctl(encoder,4010,4)){fail("Opus encoder setup failed");goto out;}
    if(capture_open(&c)||send_listen(w,sid,1))goto out;
    c.deadline=now_ms()+(int64_t)seconds*1000+2000;
    if(capture_start(&c))goto out;
    printf("{\"event\":\"recording_start\",\"seconds\":%u,\"mic_channel\":%u,\"mic_mode\":\"%s\"}\n",
           seconds,frontend.selected,channel==MIC_AUTO?"auto":"fixed");
    uint64_t left=(uint64_t)seconds*48000;
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES];uint8_t packet[1275];
    while(left) {
        unsigned frames=left<CAPTURE_CHUNK?(unsigned)left:CAPTURE_CHUNK;
        if(capture_exact(&c,raw,frames))goto out;
        memset(pcm,0,sizeof(pcm));
        unsigned previous=frontend.selected;
        unsigned n=mic_frontend_process(&frontend,raw,frames,30,pcm);
        if(n!=frames/3||n>UPLINK_SAMPLES){fail("capture resampling count mismatch");goto out;}
        if(channel==MIC_AUTO&&!frontend.locked) {
            if(frontend.score[frontend.selected]>=3.5)++onset;else onset=0;
            if(onset>=WAKE_SPEECH_PACKETS){frontend.locked=1;mic_frontend_report(&frontend,"utterance_lock");}
            else if(frontend.selected!=previous)mic_frontend_report(&frontend,"selection");
        }
        padding+=UPLINK_SAMPLES-n;
        int bytes=opus_encode(encoder,pcm[frontend.selected],UPLINK_SAMPLES,packet,sizeof(packet));
        if(bytes<=0||bytes>(int)sizeof(packet)){fail("Opus capture encode failed");goto out;}
        if(send_frame(w,2,packet,(size_t)bytes))goto out;
        ++packets;left-=frames;
    }
    /* Close the microphone immediately after the requested sample count.
     * Only the last Opus frame may contain explicitly reported zero padding. */
    if(capture_finish(&c))goto out;
    if(send_listen(w,sid,0))goto out;
    printf("{\"event\":\"recording_complete\",\"captured_frames_48000\":%llu,\"uplink_samples_16000\":%llu,"
           "\"opus_packets\":%u,\"zero_padding_samples\":%u,\"short_reads_assembled\":%u}\n",
        (unsigned long long)c.frames,(unsigned long long)(c.frames/3),packets,padding,c.short_reads);
    rc=0;
out:
    capture_close(&c);opus_encoder_destroy(encoder);return rc;
}
struct opus_slice {size_t offset,len;};
struct capture_stats {unsigned valid,auxiliary_valid,max_reason,speech_frames,vad_frames,max_silence_ms,noise_rms,threshold_rms,min_rms,max_rms;};
struct tts_queue {uint8_t *bytes;struct opus_slice *slices;size_t used;unsigned count;struct capture_stats capture_stats;};
static int queue_add(struct tts_queue *q,const uint8_t *data,size_t len) {
    if(!len||len>MAX_OPUS)return fail("buffered Opus packet length invalid");
    if(q->count>=TTS_BUFFER_PACKETS||len>TTS_BUFFER_BYTES-q->used)return fail("TTS RAM buffer limit exceeded");
    q->slices[q->count].offset=q->used;q->slices[q->count].len=len;
    memcpy(q->bytes+q->used,data,len);q->used+=len;++q->count;return 0;
}
/* VAD only rejects silence. The configured server's ASR decides whether the
 * utterance contains the wake phrase. Non-silent speech is uploaded even when
 * that phrase is absent; this is deliberately NOT a local keyword spotter. */
struct vad {
    double noise,min_rms,last_rms,last_threshold;
    unsigned streak,silence;
    int active;
    /* Created only by authorized command capture, never idle KWS. Owned by
     * the main command loop and retained across relisten/quiet reopen. */
    Fvad *speech;
};
static void vad_init(struct vad *v,unsigned min_rms) {
    memset(v,0,sizeof(*v));v->noise=8;v->min_rms=min_rms;
}
static void vad_relisten(struct vad *v) {v->streak=0;v->silence=0;v->active=0;}
static double frame_rms(const int16_t *pcm,unsigned n) {
    double sum=0,energy=0;
    for(unsigned i=0;i<n;++i)sum+=pcm[i];
    double mean=sum/n;
    for(unsigned i=0;i<n;++i){double x=pcm[i]-mean;energy+=x*x;}
    return sqrt(energy/n);
}
/* 0 waiting/continuing, 1 speech onset, 2 end after 17 silent 60 ms frames. */
static int vad_step(struct vad *v,const int16_t *pcm) {
    double rms=frame_rms(pcm,UPLINK_SAMPLES),threshold=v->noise*3.5;
    if(threshold<v->min_rms)threshold=v->min_rms;
    v->last_rms=rms;v->last_threshold=threshold;
    int voiced=rms>=threshold;
    if(!v->active) {
        if(voiced)++v->streak;
        else {
            v->streak=0;
            /* Do not let a transient inflate the noise estimate indefinitely.
             * The bounded floor persists across turns and learns only idle. */
            double sample=rms;if(sample<2)sample=2;if(sample>500)sample=500;
            v->noise=0.95*v->noise+0.05*sample;
            if(v->noise<2)v->noise=2;if(v->noise>500)v->noise=500;
        }
        if(v->streak>=WAKE_SPEECH_PACKETS){v->active=1;v->silence=0;return 1;}
    } else {
        if(voiced)v->silence=0;else ++v->silence;
        if(v->silence>=WAKE_SILENCE_PACKETS)return 2;
    }
    return 0;
}
/* A negative auxiliary result is a visible, bounded fallback to the original
 * energy endpoint; it never means silence or authorizes dropping user audio. */
static int command_speech_frames(struct vad *v,const int16_t *pcm) {
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *fault=getenv("S12_TEST_SPEECH_VAD");
    if(fault&&!strcmp(fault,"quiet"))return 0;
    if(fault&&!strcmp(fault,"speech"))return 6;
    if(fault&&(!strcmp(fault,"error")||(!strcmp(fault,"error-active")&&v->active)))goto failed;
#endif
    if(!v->speech) {
        v->speech=fvad_new();
        if(!v->speech||fvad_set_mode(v->speech,0)||fvad_set_sample_rate(v->speech,16000))goto failed;
    }
    unsigned speech=0;
    for(unsigned at=0;at<UPLINK_SAMPLES;at+=160) {
        int decision=fvad_process(v->speech,pcm+at,160);
        if(decision<0||decision>1)goto failed;
        speech+=(unsigned)decision;
    }
    return (int)speech;
failed:
    if(v->speech){fvad_free(v->speech);v->speech=NULL;}
    puts("{\"event\":\"speech_vad_fallback\",\"bounded_energy_fallback\":true,\"audio_discarded\":false}");
    return -1;
}
static unsigned capture_rms_number(double rms) {return rms<=0?0:rms>=32768?32768:(unsigned)lrint(rms);}
static void capture_stats_packet(struct tts_queue *q,const int16_t *pcm,unsigned speech,int auxiliary_valid,int classify_count) {
    struct capture_stats *stats=&q->capture_stats;unsigned rms=capture_rms_number(frame_rms(pcm,UPLINK_SAMPLES));
    if(rms<stats->min_rms)stats->min_rms=rms;if(rms>stats->max_rms)stats->max_rms=rms;
    if(!auxiliary_valid||!stats->auxiliary_valid){stats->auxiliary_valid=0;stats->speech_frames=stats->vad_frames=0;}
    else if(classify_count){stats->vad_frames+=6;stats->speech_frames+=speech;}
}
static double middle_rms(double a,double b,double c) {
    if(a>b){double t=a;a=b;b=t;}if(b>c){double t=b;b=c;c=t;}return a>b?a:b;
}
static int encode_queued(OpusEncoder *e,struct tts_queue *q,const int16_t *pcm) {
    if(q->count>=WAKE_MAX_PACKETS)return fail("utterance packet limit exceeded");
    uint8_t packet[1275];int n=opus_encode(e,pcm,UPLINK_SAMPLES,packet,sizeof(packet));
    if(n<=0||n>(int)sizeof(packet))return fail("Opus utterance encode failed");
    return queue_add(q,packet,(size_t)n);
}
#define LOCAL_RING_FRAMES 320u
#define LOCAL_CONTINUATION_GRACE 60u
#define LOCAL_WORD_TAIL_GUARD 30u
#define LOCAL_EMIT_EDGE_GUARD 8u
#define LOCAL_CONTINUATION_ONSET (WAKE_SPEECH_PACKETS*6u)
#define LOCAL_COLD_PREROLL_PACKETS 20u
struct local_pcm_ring {
    int16_t pcm[LOCAL_RING_FRAMES][160];
    uint64_t ids[LOCAL_RING_FRAMES];
    unsigned next,count;
};
static void local_ring_push(struct local_pcm_ring *r,uint64_t id,const int16_t *pcm) {
    memcpy(r->pcm[r->next],pcm,320);r->ids[r->next]=id;
    r->next=(r->next+1)%LOCAL_RING_FRAMES;if(r->count<LOCAL_RING_FRAMES)++r->count;
}
static uint64_t local_ring_latest(const struct local_pcm_ring *r) {
    return r->count?r->ids[(r->next+LOCAL_RING_FRAMES-1)%LOCAL_RING_FRAMES]:0;
}
static int local_ring_index(const struct local_pcm_ring *r,uint64_t id) {
    uint64_t last=local_ring_latest(r);
    if(!r->count||id>last||last-id>=r->count)return -1;
    unsigned index=(r->next+LOCAL_RING_FRAMES-1-(unsigned)(last-id))%LOCAL_RING_FRAMES;
    return r->ids[index]==id?(int)index:-1;
}
static const int16_t *local_ring_get(const struct local_pcm_ring *r,uint64_t id) {
    int index=local_ring_index(r,id);return index<0?NULL:r->pcm[index];
}
struct local_chain {
    s12_vendor_wake *wake,*diversity_wake;
    s12_vendor_frontend *afe;
    int16_t input[MIC_CHANNELS][128],output[UPLINK_SAMPLES*2];
    int16_t raw[MIC_CHANNELS][UPLINK_SAMPLES*2];
    unsigned input_used,output_used,raw_used;
    int command_mode;
    const char *hit_source;
    struct local_pcm_ring direct_ring,beam_ring;
    int16_t direct_channels[MIC_CHANNELS][LOCAL_RING_FRAMES][160];
    unsigned char direct_selected[LOCAL_RING_FRAMES];
    struct s12_vendor_wake_event hit_event;
    unsigned hit_channel;
    uint64_t direct_frames,beam_frames;
    unsigned ignored_hits[2],paused_models;
    /* AFE gain is independent of every physical microphone's gain. Its
     * verified ambient prior belongs only to this AFE instance. */
    double beam_noise;
    unsigned beam_noise_ready,beam_quiet_blocks,beam_noise_used;
    int beam_can_learn;
    int16_t beam_noise_pcm[UPLINK_SAMPLES];
};
#define LOCAL_FEED_KWS 1u
#define LOCAL_FEED_QUEUE 2u
#define LOCAL_FEED_RECORD 4u
#define LOCAL_FEED_BEAM_ONLY 8u
#define LOCAL_FEED_DIVERSITY_ONLY 16u
#define LOCAL_PAUSED_BEAM 1u
#define LOCAL_PAUSED_DIVERSITY 2u
static void local_chain_drop(struct local_chain *chain) {
    s12_vendor_frontend_close(chain->afe);
    s12_vendor_wake_close(chain->diversity_wake);s12_vendor_wake_close(chain->wake);
    wipe(chain,sizeof(*chain));
}
static int local_chain_models(struct local_chain *chain) {
    char error[256];
    if(vendor_identity_verified())return -1;
    chain->wake=s12_vendor_wake_open(error,sizeof(error));
    if(!chain->wake)return fail("verified original wake engine initialization failed");
    chain->diversity_wake=s12_vendor_wake_open(error,sizeof(error));
    if(!chain->diversity_wake){local_chain_drop(chain);return fail("verified diversity wake engine initialization failed");}
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    extern void s12_test_wake_role(s12_vendor_wake *,unsigned);
    s12_test_wake_role(chain->wake,0);s12_test_wake_role(chain->diversity_wake,1);
#endif
    return 0;
}
static int local_chain_reopen(struct local_chain *chain) {
    s12_vendor_frontend_close(chain->afe);chain->afe=NULL;
    /* A paused decoder cannot concatenate the PCM before and after an entire
     * playback gap. Only a fresh capture reaches here, after AEC/worker close;
     * rebuild that decoder and give its new instance a genuine zero clock. */
    char refresh_error[256];
    if(chain->paused_models&LOCAL_PAUSED_BEAM) {
        s12_vendor_wake_close(chain->wake);chain->wake=NULL;chain->beam_frames=0;
        chain->wake=s12_vendor_wake_open(refresh_error,sizeof(refresh_error));
        if(!chain->wake)return fail("paused beam wake engine reinitialization failed");
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
        extern void s12_test_wake_role(s12_vendor_wake *,unsigned);s12_test_wake_role(chain->wake,0);
#endif
        chain->paused_models&=~LOCAL_PAUSED_BEAM;
    }
    if(chain->paused_models&LOCAL_PAUSED_DIVERSITY) {
        s12_vendor_wake_close(chain->diversity_wake);chain->diversity_wake=NULL;chain->direct_frames=0;
        chain->diversity_wake=s12_vendor_wake_open(refresh_error,sizeof(refresh_error));
        if(!chain->diversity_wake)return fail("paused diversity wake engine reinitialization failed");
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
        extern void s12_test_wake_role(s12_vendor_wake *,unsigned);s12_test_wake_role(chain->diversity_wake,1);
#endif
        chain->paused_models&=~LOCAL_PAUSED_DIVERSITY;
    }
    wipe(chain->input,sizeof(chain->input));wipe(chain->output,sizeof(chain->output));wipe(chain->raw,sizeof(chain->raw));
    chain->input_used=chain->output_used=chain->raw_used=0;
    chain->command_mode=-1;chain->hit_source=NULL;
    wipe(&chain->direct_ring,sizeof(chain->direct_ring));wipe(&chain->beam_ring,sizeof(chain->beam_ring));
    wipe(chain->direct_channels,sizeof(chain->direct_channels));wipe(chain->direct_selected,sizeof(chain->direct_selected));
    memset(&chain->hit_event,0,sizeof(chain->hit_event));chain->hit_channel=0;
    chain->ignored_hits[0]=chain->ignored_hits[1]=0;
    chain->beam_quiet_blocks=chain->beam_noise_used=0;chain->beam_can_learn=0;
    /* Every newly created AFE must verify its own ambient distribution. */
    chain->beam_noise=0;chain->beam_noise_ready=0;
    wipe(chain->beam_noise_pcm,sizeof(chain->beam_noise_pcm));
    char error[256];chain->afe=s12_vendor_frontend_open(chain->wake,error,sizeof(error));
    if(!chain->afe) {
        chain->beam_noise=0;chain->beam_noise_ready=0;
        /* A valid original KWS remains mandatory. Explicit diversity fallback
         * still uses all six independent FIR/health/SNR estimates; never mix
         * unsynchronized waveforms or upload unrecognized idle speech. */
        puts("{\"event\":\"mic_frontend_fallback\",\"mode\":\"six_channel_selection\"}");
    }
    return 0;
}
static int local_chain_output(struct local_chain *chain,const int16_t *mono,unsigned flags) {
    if(chain->afe) {
    memcpy(chain->beam_noise_pcm+chain->beam_noise_used,mono,320);chain->beam_noise_used+=160;
    if(chain->beam_noise_used==UPLINK_SAMPLES) {
        /* Require 300ms of corroborating raw quiet, covering reframing and
         * short AFE tails. A BF rise can be weak speech that the raw channel
         * barely sees: never learn that rise into an established noise floor. */
        double sample=mic_rms(chain->beam_noise_pcm,UPLINK_SAMPLES);
        if(chain->beam_can_learn&&chain->beam_quiet_blocks>=5&&
           (!chain->beam_noise_ready||sample<=chain->beam_noise*1.8)) {
            if(sample<2)sample=2;if(sample>12000)sample=12000;
            if(!chain->beam_noise_ready) {
                chain->beam_noise=sample;chain->beam_noise_ready=1;
                printf("{\"event\":\"local_bf_noise_ready\",\"noise_rms\":%.2f,\"raw_quiet_ms\":%u}\n",sample,chain->beam_quiet_blocks*60);
            } else chain->beam_noise+=(sample<chain->beam_noise?.10:.025)*(sample-chain->beam_noise);
        }
        chain->beam_noise_used=0;wipe(chain->beam_noise_pcm,sizeof(chain->beam_noise_pcm));
    }
    }
    if((flags&LOCAL_FEED_KWS)&&!(flags&LOCAL_FEED_DIVERSITY_ONLY)) {
        struct s12_vendor_wake_event event;
        int r=s12_vendor_wake_process_event(chain->wake,mono,&event);
        if(r<0)return fail("original local wake processing failed");
        chain->beam_frames=event.input_frames;
        if(flags&LOCAL_FEED_RECORD) {
            local_ring_push(&chain->beam_ring,event.input_frames,mono);
            if(r&&!chain->hit_source){chain->hit_source="original_beamformer";chain->hit_event=event;}
        } else if(r)++chain->ignored_hits[1];
    }
    if(!(flags&LOCAL_FEED_QUEUE))return 0;
    if(chain->output_used+160>sizeof(chain->output)/sizeof(chain->output[0]))return fail("local frontend queue exceeded");
    memcpy(chain->output+chain->output_used,mono,320);chain->output_used+=160;return 0;
}
static int local_chain_feed(struct local_chain *chain,int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES],unsigned selected,unsigned flags,
                            const struct mic_frontend *frontend) {
    if((flags&LOCAL_FEED_BEAM_ONLY)&&(!chain->afe||(flags&LOCAL_FEED_DIVERSITY_ONLY)))
        return fail("fixed monitor beam source unavailable");
    unsigned healthy=0;int quiet=1;
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)if(frontend->healthy[ch]) {
        ++healthy;
        if(mic_rms(pcm[ch],UPLINK_SAMPLES)>frontend->noise[ch]*1.8+1)quiet=0;
    }
    chain->beam_can_learn=!frontend->locked;
    if(quiet&&healthy>=2){if(chain->beam_quiet_blocks<100)++chain->beam_quiet_blocks;}
    else chain->beam_quiet_blocks=0;
    if(flags&LOCAL_FEED_QUEUE) {
        if(chain->raw_used+UPLINK_SAMPLES>UPLINK_SAMPLES*2)return fail("microphone alignment queue exceeded");
        for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(chain->raw[ch]+chain->raw_used,pcm[ch],UPLINK_SAMPLES*2);
        chain->raw_used+=UPLINK_SAMPLES;
    }
    /* The array's real acoustic geometry/phase behavior still needs a live
     * voice check. Keep a completely independent original-model detector on
     * one health/SNR-selected channel, so BF attenuation cannot hide a word. */
    if((flags&LOCAL_FEED_KWS)&&!(flags&LOCAL_FEED_BEAM_ONLY))for(unsigned at=0;at<UPLINK_SAMPLES;at+=160) {
        struct s12_vendor_wake_event event;
        int r=s12_vendor_wake_process_event(chain->diversity_wake,pcm[selected]+at,&event);
        if(r<0)return fail("original diversity wake processing failed");
        chain->direct_frames=event.input_frames;
        if(flags&LOCAL_FEED_RECORD) {
            unsigned index=chain->direct_ring.next;
            for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(chain->direct_channels[ch][index],pcm[ch]+at,320);
            chain->direct_selected[index]=(unsigned char)selected;
            local_ring_push(&chain->direct_ring,event.input_frames,pcm[selected]+at);
            if(r&&!chain->hit_source){chain->hit_source="six_channel_selection";chain->hit_event=event;chain->hit_channel=selected;}
        } else if(r)++chain->ignored_hits[0];
    }
    /* Fixed playback monitoring authorizes only the diversity KWS. With
     * no output queue the AFE result has no consumer; do not spend realtime
     * CPU on an unused beam. Its decoder is already paused and the next
     * ordinary capture rebuilds that AFE before feeding either beam path. */
    if((flags&LOCAL_FEED_DIVERSITY_ONLY)&&!(flags&LOCAL_FEED_QUEUE))return 0;
    if(!chain->afe) {
        if(!(flags&LOCAL_FEED_QUEUE))return 0;
        /* No BF output exists in fallback mode. Queue the selected mic, but
         * never invent BF frames or substitute raw PCM into its KWS clock. */
        for(unsigned at=0;at<UPLINK_SAMPLES;at+=160){int r=local_chain_output(chain,pcm[selected]+at,LOCAL_FEED_QUEUE);if(r)return r;}
        return 0;
    }
    unsigned at=0;
    while(at<UPLINK_SAMPLES) {
        unsigned count=128-chain->input_used;if(count>UPLINK_SAMPLES-at)count=UPLINK_SAMPLES-at;
        for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(chain->input[ch]+chain->input_used,pcm[ch]+at,count*2);
        at+=count;chain->input_used+=count;
        if(chain->input_used==128) {
            int16_t out[160];int r=s12_vendor_frontend_process(chain->afe,chain->input,out);chain->input_used=0;
            if(r<0)return fail("original microphone frontend contract failed");
            if(r){r=local_chain_output(chain,out,flags);wipe(out,sizeof(out));if(r)return r;}
        }
    }
    return 0;
}
static void local_chain_consume(struct local_chain *chain) {
    chain->output_used-=UPLINK_SAMPLES;chain->raw_used-=UPLINK_SAMPLES;
    memmove(chain->output,chain->output+UPLINK_SAMPLES,chain->output_used*2);
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memmove(chain->raw[ch],chain->raw[ch]+UPLINK_SAMPLES,chain->raw_used*2);
}
static const int16_t *local_chain_saved(const struct local_chain *chain,const struct local_pcm_ring *ring,uint64_t id) {
    int index=local_ring_index(ring,id);if(index<0)return NULL;
    return ring==&chain->direct_ring?chain->direct_channels[chain->hit_channel][index]:ring->pcm[index];
}
/* Aggregate diagnostics only. No PCM, words, remote data or credentials
 * enter this fixed private RAM file. Write failures cannot stop the audio
 * path; periodic updates are limited to1Hz, with immediate state changes. */
struct barge_status_record {
    const char *state;
    uint64_t monitor_starts,capture_blocks,keyword_hits,capture_discontinuities,old_turns_dropped;
    uint64_t candidate_hits,reference_hits,self_echo_vetoes,playback_xruns;
    uint64_t candidate_begin_frame,candidate_end_frame,candidate_emit_frame;
    uint64_t reference_begin_frame,reference_end_frame,reference_emit_frame;
    const char *last_decision,*last_turn_error,*last_error_reason;
    int conversation_active;
    uint64_t followup_windows,followup_turns;
    unsigned last_dsp_ms,max_dsp_ms,capture_backlog_frames,reference_dsp_ms;
    unsigned playback_buffer_frames,playback_period_frames;
    int64_t written;
};
static struct barge_status_record barge_main_status;
/* A short registry lookup selects per-thread diagnostic storage. This avoids
 * adding an ARM TLS ABI dependency and never holds a lock across vendor DSP. */
static pthread_mutex_t barge_registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_t barge_registry_thread;
static struct barge_status_record *barge_registry_status;
static int barge_registry_valid;
static int barge_in_worker(void) {
    pthread_mutex_lock(&barge_registry_mutex);
    int yes=barge_registry_valid&&pthread_equal(pthread_self(),barge_registry_thread);
    pthread_mutex_unlock(&barge_registry_mutex);return yes;
}
static struct barge_status_record *barge_status_for_caller(void) {
    pthread_mutex_lock(&barge_registry_mutex);
    struct barge_status_record *value=barge_registry_valid&&pthread_equal(pthread_self(),barge_registry_thread)?barge_registry_status:&barge_main_status;
    pthread_mutex_unlock(&barge_registry_mutex);return value;
}
#define barge_status (*barge_status_for_caller())
static void barge_count(uint64_t *counter) {if(*counter<UINT64_C(9007199254740991))++*counter;}
static void barge_note_failure(const char *stage) {
    if(barge_in_worker())return; /* Worker errors use the bounded mailbox. */
    if(strstr(stage,"playback")||strstr(stage,"ALSA drain"))barge_failure="playback";
    else if(strstr(stage,"capture")||strstr(stage,"microphone"))barge_failure="capture";
    else if(strstr(stage,"socket")||strstr(stage,"server closed")||strstr(stage,"connect")||strstr(stage,"HTTP")||
            strstr(stage,"websocket")||strstr(stage,"WebSocket")||strstr(stage,"handshake")||!strcmp(stage,"deadline expired"))barge_failure="network";
    else if(strstr(stage,"JSON")||strstr(stage,"server")||strstr(stage,"TTS")||strstr(stage,"music")||strstr(stage,"Opus packet"))barge_failure="protocol";
    else barge_failure="other";
    if(!strcmp(stage,"ALSA playback XRUN"))barge_error_reason="playback_xrun";
    else if(strstr(stage,"peer closed")||strstr(stage,"server closed")||strstr(stage,"socket closed"))barge_error_reason="peer_closed";
    else if(strstr(stage,"deadline")||strstr(stage,"timed out"))barge_error_reason="timeout";
    else if(strstr(stage,"connect"))barge_error_reason="connect";
    else if(strstr(stage,"handshake")||strstr(stage,"HTTP")||strstr(stage,"WebSocket upgrade"))barge_error_reason="handshake";
    else if(!strcmp(barge_failure,"playback"))barge_error_reason="playback";
    else if(!strcmp(barge_failure,"protocol"))barge_error_reason="protocol";
    else barge_error_reason="other";
    barge_status.last_error_reason=barge_error_reason;
    if(!strcmp(stage,"ALSA playback XRUN"))barge_count(&barge_status.playback_xruns);
}
#ifndef XIAOZHI_TEST_NO_ALSA
static void barge_playback_settings(unsigned buffer,unsigned period) {
    barge_status.playback_buffer_frames=buffer>480000?480000:buffer;
    barge_status.playback_period_frames=period>480000?480000:period;
}
#endif
static void barge_status_write(const char *state,int force) {
    if(!barge_config.enabled)return;
    if(barge_in_worker()){barge_status.state=state;return;} /* Publish only; no worker filesystem I/O. */
    int64_t now=now_ms();
    if(!barge_status.state||strcmp(barge_status.state,state)){barge_status.state=state;force=1;}
    if(!force&&now>=0&&now-barge_status.written<1000)return;
    barge_status.written=now;
    char path[4096],temporary[4096],raw[2048];
    if(panel_path(path,sizeof(path),"barge-status.json")||panel_path(temporary,sizeof(temporary),"barge-status.client-new"))return;
    int n=snprintf(raw,sizeof(raw),"{\"schema\":3,\"enabled\":true,\"state\":\"%s\",\"monitor_starts\":%llu,\"capture_blocks\":%llu,\"keyword_hits\":%llu,\"capture_discontinuities\":%llu,\"old_turns_dropped\":%llu,\"reference_channel\":%u,\"reference_gain_permille\":%u,\"last_dsp_ms\":%u,\"max_dsp_ms\":%u,\"candidate_hits\":%llu,\"reference_hits\":%llu,\"self_echo_vetoes\":%llu,\"last_decision\":\"%s\",\"candidate_begin_frame\":%llu,\"candidate_end_frame\":%llu,\"candidate_emit_frame\":%llu,\"reference_begin_frame\":%llu,\"reference_end_frame\":%llu,\"reference_emit_frame\":%llu,\"last_turn_error\":\"%s\",\"playback_xruns\":%llu,\"capture_backlog_frames\":%u,\"reference_dsp_ms\":%u,\"playback_buffer_frames\":%u,\"playback_period_frames\":%u,\"conversation_active\":%s,\"followup_windows\":%llu,\"followup_turns\":%llu,\"last_error_reason\":\"%s\"}\n",
        state,(unsigned long long)barge_status.monitor_starts,(unsigned long long)barge_status.capture_blocks,
        (unsigned long long)barge_status.keyword_hits,(unsigned long long)barge_status.capture_discontinuities,
        (unsigned long long)barge_status.old_turns_dropped,barge_effective_reference(),barge_config.gain,barge_status.last_dsp_ms,barge_status.max_dsp_ms,
        (unsigned long long)barge_status.candidate_hits,(unsigned long long)barge_status.reference_hits,(unsigned long long)barge_status.self_echo_vetoes,
        barge_status.last_decision?barge_status.last_decision:"none",
        (unsigned long long)barge_status.candidate_begin_frame,(unsigned long long)barge_status.candidate_end_frame,(unsigned long long)barge_status.candidate_emit_frame,
        (unsigned long long)barge_status.reference_begin_frame,(unsigned long long)barge_status.reference_end_frame,(unsigned long long)barge_status.reference_emit_frame,
        barge_status.last_turn_error?barge_status.last_turn_error:"none",(unsigned long long)barge_status.playback_xruns,
        barge_status.capture_backlog_frames,barge_status.reference_dsp_ms,barge_status.playback_buffer_frames,barge_status.playback_period_frames,
        barge_status.conversation_active?"true":"false",(unsigned long long)barge_status.followup_windows,
        (unsigned long long)barge_status.followup_turns,barge_status.last_error_reason?barge_status.last_error_reason:"none");
    if(n<=0||(size_t)n>=sizeof(raw))return;
    int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if(fd<0&&errno==EEXIST){unlink(temporary);fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);}
    if(fd<0)return;
    int good=write(fd,raw,(size_t)n)==n;if(close(fd))good=0;
    if(good){if(rename(temporary,path))unlink(temporary);}else unlink(temporary);
}
static void barge_dsp_duration(int64_t begin) {
    int64_t end=now_ms(),elapsed=end>=begin?end-begin:0;
    if(elapsed>10000)elapsed=10000;barge_status.last_dsp_ms=(unsigned)elapsed;
    if(barge_status.last_dsp_ms>barge_status.max_dsp_ms)barge_status.max_dsp_ms=barge_status.last_dsp_ms;
}
/* One capture owner remains alive across media playback and a confirmed
 * interruption. One worker owns all DSP/ring IDs until main joins it for a
 * handoff; the reader thread merely transports bounded raw frames. No idle PCM goes
 * to the server. A poisoned AEC never falls back to echo-containing raw KWS. */
enum {BARGE_SOURCE_BEAM=1,BARGE_SOURCE_DIVERSITY=2};
struct barge_monitor {
    unsigned monitor_source;
    /* Only mailbox/control/snapshot fields are shared while worker_started.
     * All remaining DSP/chain/capture-consumer fields belong to the worker. */
    pthread_t worker;
    pthread_mutex_t worker_mutex;
    int worker_mutex_ready,worker_started;
    int worker_stop,worker_cancel,worker_eof,worker_exited,worker_event;
    uint64_t worker_eof_target;
    int published_candidate;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    int worker_test_after_hit;
#endif
    struct barge_status_record worker_status,published_status;
    struct capture capture;
    struct mic_frontend frontend;
    struct decimator reference_filter;
    s12_vendor_aec *aec;
    s12_vendor_wake *reference_wake;
    struct local_chain *chain;
    uint64_t direct_epoch,beam_epoch,reference_frames,reference_model_epoch;
    unsigned reference_zero_frames;
    int reference_model_started,reference_needs_reset;
    struct s12_vendor_wake_event reference_events[16];
    unsigned reference_next,reference_count;
    struct {uint64_t frame;unsigned char nonzero;} reference_activity[LOCAL_RING_FRAMES];
    struct {
        int valid;
        const char *source;
        unsigned channel;
        struct s12_vendor_wake_event original,relative;
        uint64_t deadline;
        int64_t wall_deadline;
    } candidate;
    unsigned min_rms,input_used,clean_used;
    double idle_beam_noise;
    unsigned idle_beam_ready;
    int opened,pending,failed,pumping,prior_saved;
    int16_t input[MIC_CHANNELS][128],reference[128];
    int16_t clean[MIC_CHANNELS][UPLINK_SAMPLES*2];
    int16_t clean_reference[UPLINK_SAMPLES*2],block_reference[UPLINK_SAMPLES];
};
static struct barge_monitor *barge_current;
enum {BARGE_WORKER_NONE,BARGE_WORKER_HIT,BARGE_WORKER_FAILED,BARGE_WORKER_EOF,BARGE_WORKER_CANCELLED};
static int barge_worker_start(struct barge_monitor *);
static void barge_worker_join(struct barge_monitor *,int);
static int barge_worker_cancelled(struct barge_monitor *b) {
    if(!barge_in_worker())return stopping||panel_interrupted||(panel_enabled&&panel.muted)||ota_idle_requested();
    pthread_mutex_lock(&b->worker_mutex);int value=b->worker_stop||b->worker_cancel;
    pthread_mutex_unlock(&b->worker_mutex);return value;
}
static void barge_select_source(struct barge_monitor *b) {
    /* Playback uses the already validated diversity wake path. All six AEC
     * and health/SNR streams remain live; AFE may still process PCM, but its
     * separate decoder is paused until a fresh capture rebuilds it. Original
     * BF ABI/reframing is verified; BF acoustic wake quality is not yet. */
    b->monitor_source=BARGE_SOURCE_DIVERSITY;
    b->chain->paused_models|=LOCAL_PAUSED_BEAM;
}
static unsigned barge_feed_flags(const struct barge_monitor *b) {
    if(b->monitor_source==BARGE_SOURCE_BEAM)return LOCAL_FEED_KWS|LOCAL_FEED_RECORD|LOCAL_FEED_BEAM_ONLY;
    if(b->monitor_source==BARGE_SOURCE_DIVERSITY)return LOCAL_FEED_KWS|LOCAL_FEED_RECORD|LOCAL_FEED_DIVERSITY_ONLY;
    return 0;
}
static uint64_t barge_model_progress(const struct barge_monitor *b) {
    return b->monitor_source==BARGE_SOURCE_BEAM?b->chain->beam_frames-b->beam_epoch:b->chain->direct_frames-b->direct_epoch;
}
/* Only the independent playback frontend sees4x microphone PCM. Its copied
 * ordinary noise prior must use the same units; never write it back. */
static void barge_frontend_input_units(struct mic_frontend *m) {
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch){double n=m->noise[ch]*4;if(!(n>=2))n=2;else if(n>12000)n=12000;m->noise[ch]=n;}
}
static void barge_metrics(struct mic_frontend *m,int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES],unsigned minimum) {
    unsigned best=m->selected;double best_score=-1;
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch) {
        m->rms[ch]=mic_rms(pcm[ch],UPLINK_SAMPLES);unsigned clipped=0;
        for(unsigned i=0;i<UPLINK_SAMPLES;++i)if(pcm[ch][i]>32275||pcm[ch][i]<-32275)++clipped;
        m->clip[ch]=(double)clipped/UPLINK_SAMPLES;
        m->healthy[ch]=(unsigned char)(m->rms[ch]>=1&&m->clip[ch]<=.005);
        double floor=m->noise[ch];if(floor<(double)minimum/3.5)floor=(double)minimum/3.5;
        m->score[ch]=m->healthy[ch]?m->rms[ch]/floor:0;
        if(m->score[ch]>best_score){best_score=m->score[ch];best=ch;}
    }
    if(!m->locked) {
        if(best!=m->selected&&m->healthy[best]&&(!m->healthy[m->selected]||best_score>m->score[m->selected]*1.35)) {
            if(m->challenger!=best){m->challenger=best;m->challenge_frames=1;}else ++m->challenge_frames;
            if(!m->healthy[m->selected]||best_score>=3.5||m->challenge_frames>=3){m->selected=best;m->challenge_frames=0;}
        } else m->challenge_frames=0;
        if(best_score<2.5)for(unsigned ch=0;ch<MIC_CHANNELS;++ch) {
            double sample=m->rms[ch];if(sample<2)sample=2;if(sample>12000)sample=12000;
            m->noise[ch]+=(sample<m->noise[ch]?.10:.015)*(sample-m->noise[ch]);
        }
    }
    ++m->blocks;
}
static int barge_stop(struct barge_monitor *b) {
    if(!b)return 0;barge_active=0;
    barge_worker_join(b,1); /* Ownership transfers only after the worker exits. */
    int had_resources=b->opened||b->aec||b->reference_wake||b->pending||b->failed;
    int result=b->opened?capture_finish(&b->capture):0;b->opened=0;
    int already_failed=!!b->failed;
    if(s12_vendor_aec_close(b->aec)&&!result)result=CAPTURE_DISCONTINUITY;
    b->aec=NULL;s12_vendor_wake_close(b->reference_wake);b->reference_wake=NULL;
    if(b->candidate.valid){b->candidate.valid=0;barge_status.last_decision="discarded";}
    b->pending=0;b->failed=0;
    wipe(b->input,sizeof(b->input));wipe(b->reference,sizeof(b->reference));wipe(b->clean,sizeof(b->clean));
    b->input_used=b->clean_used=0;
    wipe(b->clean_reference,sizeof(b->clean_reference));wipe(b->block_reference,sizeof(b->block_reference));
    wipe(b->reference_activity,sizeof(b->reference_activity));wipe(b->reference_events,sizeof(b->reference_events));
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)wipe(b->frontend.filters[ch].history,sizeof(b->frontend.filters[ch].history));
    wipe(b->reference_filter.history,sizeof(b->reference_filter.history));
    if(b->capture.fifo_peak>barge_status.capture_backlog_frames)barge_status.capture_backlog_frames=b->capture.fifo_peak;
    if(b->chain) {
        struct local_chain *chain=b->chain;
        /* The direct decoder consumed the monitor's4x PCM. Rebuild only on
         * the next ordinary reopen, after any same-sentence PCM was copied. */
        if(had_resources)chain->paused_models|=LOCAL_PAUSED_DIVERSITY;
        wipe(chain->input,sizeof(chain->input));wipe(chain->output,sizeof(chain->output));wipe(chain->raw,sizeof(chain->raw));
        wipe(&chain->direct_ring,sizeof(chain->direct_ring));wipe(&chain->beam_ring,sizeof(chain->beam_ring));
        wipe(chain->direct_channels,sizeof(chain->direct_channels));wipe(chain->direct_selected,sizeof(chain->direct_selected));
        wipe(&chain->hit_event,sizeof(chain->hit_event));wipe(chain->beam_noise_pcm,sizeof(chain->beam_noise_pcm));
        chain->input_used=chain->output_used=chain->raw_used=chain->beam_noise_used=0;
        chain->hit_source=NULL;chain->beam_noise=b->prior_saved?b->idle_beam_noise:0;
        chain->beam_noise_ready=b->prior_saved?b->idle_beam_ready:0;
    }
    if(result&&!already_failed)barge_count(&barge_status.capture_discontinuities);
    if(had_resources)barge_status_write(result||already_failed?"error":panel_enabled&&panel.muted?"muted":"idle",0);
    return result;
}
/* OTA's idle acknowledgment is reached only after the monitor reader has
 * joined and all PCM/preroll was erased. A mute likewise wins over a pending
 * keyword; neither condition authorizes a new command upload. */
static int barge_control_handoff(struct barge_monitor *b) {
    if(b&&b->pending&&(ota_idle_requested()||(panel_enabled&&panel.muted)))return barge_stop(b);
    return 0;
}
static int barge_start(struct barge_monitor *b,struct local_chain *chain,const struct mic_frontend *frontend,unsigned minimum) {
    memset(b,0,sizeof(*b));b->chain=chain;b->frontend=*frontend;b->min_rms=minimum;
    barge_frontend_input_units(&b->frontend);
    b->idle_beam_noise=chain->beam_noise;b->idle_beam_ready=chain->beam_noise_ready;b->prior_saved=1;
    b->direct_epoch=chain->direct_frames;b->beam_epoch=chain->beam_frames;
    mic_frontend_reopen(&b->frontend);decimator_init(&b->reference_filter);
    /* Monitor4x PCM starts a genuine direct-model epoch, independent of the
     * preceding ordinary1x capture. No live handoff decoder is rebuilt. */
    chain->paused_models|=LOCAL_PAUSED_DIVERSITY;
    if(local_chain_reopen(chain))return -1;
    b->direct_epoch=chain->direct_frames;b->beam_epoch=chain->beam_frames;barge_select_source(b);
    /* AEC changes the BF noise distribution. The ordinary raw-capture prior
     * must not be mistaken for a measured clean-stream silence estimate. */
    chain->beam_noise=0;chain->beam_noise_ready=0;
    char error[256];b->aec=s12_vendor_aec_open(chain->wake,error,sizeof(error));
    if(!b->aec){barge_status_write("error",0);return fail("verified barge AEC initialization failed");}
    b->reference_wake=s12_vendor_wake_open(error,sizeof(error));
    if(!b->reference_wake){(void)barge_stop(b);barge_status_write("error",0);return fail("reference wake engine initialization failed");}
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    extern void s12_test_reference_wake(s12_vendor_wake *);s12_test_reference_wake(b->reference_wake);
#endif
    b->opened=1;
    if(capture_open(&b->capture)){(void)barge_stop(b);barge_status_write("error",0);return -1;}
    int result=capture_start(&b->capture);
    if(result){(void)barge_stop(b);if(result==CAPTURE_DISCONTINUITY)barge_count(&barge_status.capture_discontinuities);barge_status_write("error",0);return result;}
    barge_current=b;barge_active=1;
    barge_count(&barge_status.monitor_starts);barge_status_write("monitoring",0);
    if(barge_worker_start(b)){(void)barge_stop(b);return fail("barge worker start failed");}
    printf("{\"event\":\"barge_monitor_started\",\"reference_channel\":%u,\"microphones\":6,\"idle_audio_uploaded\":false,\"capture_owner\":\"single_reader\"}\n",barge_effective_reference());
    return 0;
}
/* Return one60ms AEC-clean block, 1 if a nonblocking read is not ready, or
 * a discontinuity/error. 128-sample AEC framing carries across960-sample KWS
 * blocks, preserving every sample and each detector's own10ms frame axis. */
/* Keep originalS32 lowbits and the same
 * normalized63tap FIR. All6mic planes enter AEC at4x; this is not a gain on
 * KWS output. Saturate afterFIR, never copy OEMstrh integerwrap. */
static double barge_mic_input(int32_t value){return (double)value/16384.0;}
static int16_t barge_mic_s16(double value){if(value>32767)return 32767;if(value< -32768)return -32768;return (int16_t)lrint(value);}
static unsigned barge_mic_decimate(struct decimator *d,const int32_t *interleaved,unsigned frames,unsigned channel,int16_t *out) {
    if(channel>=MIC_CHANNELS)return 0;unsigned produced=0;
    for(unsigned i=0;i<frames;++i){d->history[d->pos]=barge_mic_input(interleaved[i*CAPTURE_CHANNELS+channel]);d->pos=(d->pos+1)%FILTER_TAPS;if(++d->phase!=3)continue;d->phase=0;double y=0;for(unsigned k=0;k<FILTER_TAPS;++k)y+=d->taps[k]*d->history[(d->pos+FILTER_TAPS-1-k)%FILTER_TAPS];out[produced++]=barge_mic_s16(y);}return produced;
}
static int barge_next_clean(struct barge_monitor *b,int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES],int blocking) {
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];
    int16_t microphones[MIC_CHANNELS][UPLINK_SAMPLES],reference[UPLINK_SAMPLES],out[MIC_CHANNELS][128];
    int result=0;
    while(b->clean_used<UPLINK_SAMPLES) {
        if(!blocking) {
            pthread_mutex_lock(&b->capture.mutex);
            int error=b->capture.reader_error;unsigned available=b->capture.fifo_used;
            pthread_mutex_unlock(&b->capture.mutex);
            if(error){result=CAPTURE_DISCONTINUITY;goto done;}
            if(available<CAPTURE_CHUNK){result=1;goto done;}
            long count=capture_read(&b->capture,raw,CAPTURE_CHUNK);
            if(count!=CAPTURE_CHUNK){result=CAPTURE_DISCONTINUITY;goto done;}
            b->capture.frames+=CAPTURE_CHUNK;s12dbg_consumed(b->capture.debug_id,b->capture.frames);
        } else {
            b->capture.deadline=now_ms()+1500;
            result=capture_exact(&b->capture,raw,CAPTURE_CHUNK);if(result)goto done;
        }
        for(unsigned ch=0;ch<MIC_CHANNELS;++ch)
            if(barge_mic_decimate(&b->frontend.filters[ch],raw,CAPTURE_CHUNK,ch,microphones[ch])!=UPLINK_SAMPLES){result=-1;goto done;}
        if(decimate(&b->reference_filter,raw,CAPTURE_CHUNK,barge_effective_reference(),reference)!=UPLINK_SAMPLES){result=-1;goto done;}
        s12dbg_aec_raw(b->capture.debug_id,b->capture.frames,microphones,reference);
        for(unsigned at=0;at<UPLINK_SAMPLES;) {
            unsigned count=128-b->input_used;if(count>UPLINK_SAMPLES-at)count=UPLINK_SAMPLES-at;
            for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(b->input[ch]+b->input_used,microphones[ch]+at,count*2);
            for(unsigned i=0;i<count;++i) {
                int value=(int)reference[at+i]*(int)barge_config.gain/1000;
                if(value>32767)value=32767;if(value<-32768)value=-32768;
                b->reference[b->input_used+i]=(int16_t)value;
            }
            at+=count;b->input_used+=count;
            if(b->input_used==128) {
                if(s12_vendor_aec_process(b->aec,b->input,b->reference,out)){result=CAPTURE_DISCONTINUITY;goto done;}
                if(b->clean_used+128>UPLINK_SAMPLES*2){result=-1;goto done;}
                for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(b->clean[ch]+b->clean_used,out[ch],256);
                memcpy(b->clean_reference+b->clean_used,b->reference,256);
                b->clean_used+=128;b->input_used=0;
            }
        }
    }
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch) {
        memcpy(pcm[ch],b->clean[ch],UPLINK_SAMPLES*2);
        memmove(b->clean[ch],b->clean[ch]+UPLINK_SAMPLES,(b->clean_used-UPLINK_SAMPLES)*2);
    }
    memcpy(b->block_reference,b->clean_reference,UPLINK_SAMPLES*2);
    memmove(b->clean_reference,b->clean_reference+UPLINK_SAMPLES,(b->clean_used-UPLINK_SAMPLES)*2);
    b->clean_used-=UPLINK_SAMPLES;barge_metrics(&b->frontend,pcm,b->min_rms);
    s12dbg_aec_clean(b->capture.debug_id,b->reference_frames+6,pcm,b->block_reference,&b->frontend,barge_config.gain);
done:
    wipe(raw,sizeof(raw));wipe(microphones,sizeof(microphones));wipe(reference,sizeof(reference));wipe(out,sizeof(out));
    return result;
}
/* The playback reference is an independent negative witness, never a wake
 * authorization. Its third original model is allocated before capture starts.
 * Every reference sample follows the same128->960 carry as the clean mics. */
static void barge_reference_status(const struct s12_vendor_wake_event *event) {
    barge_status.reference_begin_frame=event->timing_valid?event->begin_frame:0;
    barge_status.reference_end_frame=event->timing_valid?event->end_frame:0;
    barge_status.reference_emit_frame=event->input_frames;
}
static int barge_reference_feed(struct barge_monitor *b) {
    int64_t begin=now_ms();int processed=0;
    for(unsigned at=0;at<UPLINK_SAMPLES;at+=160) {
        ++b->reference_frames;
        unsigned index=(unsigned)(b->reference_frames%LOCAL_RING_FRAMES);int nonzero=0;
        for(unsigned i=0;i<160;++i)if(b->block_reference[at+i]){nonzero=1;break;}
        b->reference_activity[index].frame=b->reference_frames;
        b->reference_activity[index].nonzero=(unsigned char)nonzero;
        if(!nonzero) {
            if(!b->reference_model_started)continue;
            if(b->reference_zero_frames>=60) {
                b->reference_model_started=0;b->reference_needs_reset=1;continue;
            }
            ++b->reference_zero_frames;
        } else {
            if(b->reference_needs_reset) {
                s12_vendor_wake_close(b->reference_wake);b->reference_wake=NULL;
                char error[256];b->reference_wake=s12_vendor_wake_open(error,sizeof(error));
                if(!b->reference_wake)return -1;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
                extern void s12_test_reference_wake(s12_vendor_wake *);s12_test_reference_wake(b->reference_wake);
#endif
                b->reference_needs_reset=0;
            }
            if(!b->reference_model_started) {
                b->reference_model_epoch=b->reference_frames-1;b->reference_model_started=1;
            }
            b->reference_zero_frames=0;
        }
        struct s12_vendor_wake_event event;
        int result=s12_vendor_wake_process_event(b->reference_wake,b->block_reference+at,&event);
        processed=1;
        if(result<0||event.input_frames+b->reference_model_epoch!=b->reference_frames)return -1;
        event.input_frames+=b->reference_model_epoch;
        if(event.timing_valid){event.begin_frame+=b->reference_model_epoch;event.end_frame+=b->reference_model_epoch;}
        if(result) {
            if(event.status!=1)return -1;
            b->reference_events[b->reference_next]=event;b->reference_next=(b->reference_next+1)%16;
            if(b->reference_count<16)++b->reference_count;
            barge_count(&barge_status.reference_hits);barge_reference_status(&event);
            barge_status_write("monitoring",1);
            /* The original Paddy engine retains a previous-hit suppression
             * range even after its partial automatic reset. This negative
             * reference witness must recognize every repeated playback word.
             * Preserve the mapped event, then replace only this model before
             * the next real nonzero frame; never replay or skip physical PCM. */
            b->reference_model_started=0;b->reference_needs_reset=1;b->reference_zero_frames=0;
        }
    }
    if(processed) {
        int64_t elapsed=now_ms()-begin;if(elapsed<0)elapsed=0;if(elapsed>10000)elapsed=10000;
        if((unsigned)elapsed>barge_status.reference_dsp_ms)barge_status.reference_dsp_ms=(unsigned)elapsed;
    }
    return 0;
}
static void barge_candidate_clear(struct barge_monitor *b,const char *decision) {
    wipe(&b->candidate,sizeof(b->candidate));barge_status.last_decision=decision;
}
static void barge_candidate_begin(struct barge_monitor *b) {
    struct local_chain *chain=b->chain;
    if(!b->candidate.valid) {
        b->candidate.valid=1;b->candidate.source=chain->hit_source;b->candidate.channel=chain->hit_channel;
        b->candidate.original=chain->hit_event;
        uint64_t epoch=!strcmp(chain->hit_source,"six_channel_selection")?b->direct_epoch:b->beam_epoch;
        struct s12_vendor_wake_event event=chain->hit_event;
        event.input_frames=event.input_frames>=epoch?event.input_frames-epoch:0;
        event.timing_valid=event.timing_valid&&event.begin_frame>=epoch&&event.end_frame>=epoch;
        event.begin_frame=event.timing_valid?event.begin_frame-epoch:0;
        event.end_frame=event.timing_valid?event.end_frame-epoch:0;
        b->candidate.relative=event;
        /* The reference has already consumed this complete60ms block. Bound
         * lookahead by sample progress, never by a delayed wall-clock pump. */
        b->candidate.deadline=event.input_frames+30;b->candidate.wall_deadline=now_ms()+2000;
        barge_count(&barge_status.candidate_hits);barge_status.last_decision="pending";
        barge_status.candidate_begin_frame=event.begin_frame;barge_status.candidate_end_frame=event.end_frame;
        barge_status.candidate_emit_frame=event.input_frames;barge_status_write("monitoring",1);
    }
    /* Continued monitoring must not retain a stale hit_source that blocks
     * future original-model events after a rejected self-echo candidate. */
    chain->hit_source=NULL;wipe(&chain->hit_event,sizeof(chain->hit_event));
}
static int barge_reference_matches(const struct barge_monitor *b,const struct s12_vendor_wake_event *ref) {
    const struct s12_vendor_wake_event *mic=&b->candidate.relative;
    if(ref->input_frames>b->candidate.deadline)return 0;
    if(mic->timing_valid&&ref->timing_valid)
        return mic->begin_frame<=ref->end_frame+30&&ref->begin_frame<=mic->end_frame+30;
    uint64_t distance=mic->input_frames>ref->input_frames?mic->input_frames-ref->input_frames:ref->input_frames-mic->input_frames;
    return distance<=30;
}
static int barge_reference_silent(const struct barge_monitor *b) {
    const struct s12_vendor_wake_event *event=&b->candidate.relative;
    uint64_t first=event->timing_valid?event->begin_frame:event->input_frames;
    first=first>30?first-30:1;
    if(b->reference_frames<first||b->reference_frames-first>=LOCAL_RING_FRAMES)return 0;
    for(uint64_t frame=first;frame<=b->reference_frames;++frame) {
        unsigned index=(unsigned)(frame%LOCAL_RING_FRAMES);
        if(b->reference_activity[index].frame!=frame||b->reference_activity[index].nonzero)return 0;
    }
    return 1;
}
/* Return1 on accepted near-end candidate,0 while pending/rejected, -1 only
 * for a hard scheduling failure. Same-word simultaneous playback/user speech
 * is deliberately conservative until its acoustic timing is validated. */
static int barge_candidate_decide(struct barge_monitor *b) {
    if(!b->candidate.valid)return 0;
    if(barge_worker_cancelled(b)) {
        barge_candidate_clear(b,"discarded");barge_status_write("monitoring",1);return 0;
    }
    for(unsigned i=0;i<b->reference_count;++i)if(barge_reference_matches(b,&b->reference_events[i])) {
        barge_reference_status(&b->reference_events[i]);barge_count(&barge_status.self_echo_vetoes);
        barge_candidate_clear(b,"self_echo");barge_status_write("monitoring",1);return 0;
    }
    if(!barge_reference_silent(b)&&b->reference_frames<b->candidate.deadline) {
        if(now_ms()>=b->candidate.wall_deadline)return -1;
        return 0;
    }
    b->chain->hit_source=b->candidate.source;b->chain->hit_event=b->candidate.original;
    b->chain->hit_channel=b->candidate.channel;barge_candidate_clear(b,"accepted");return 1;
}

static void barge_merge_snapshot(const struct barge_status_record *s) {
    /* Playback state/error counters remain exclusively main-owned and must
     * never regress when an older worker snapshot is published. */
#define COPY_DSP(field) barge_status.field=s->field
    COPY_DSP(capture_blocks);COPY_DSP(capture_discontinuities);
    COPY_DSP(candidate_hits);COPY_DSP(reference_hits);COPY_DSP(self_echo_vetoes);
    COPY_DSP(candidate_begin_frame);COPY_DSP(candidate_end_frame);COPY_DSP(candidate_emit_frame);
    COPY_DSP(reference_begin_frame);COPY_DSP(reference_end_frame);COPY_DSP(reference_emit_frame);
    COPY_DSP(last_decision);COPY_DSP(last_dsp_ms);COPY_DSP(max_dsp_ms);
    COPY_DSP(capture_backlog_frames);COPY_DSP(reference_dsp_ms);
#undef COPY_DSP
}
static void barge_worker_publish(struct barge_monitor *b,int event,int exited) {
    pthread_mutex_lock(&b->worker_mutex);
    /* Controls can arrive after the last DSP decision. Publish the event
     * under the same sticky-control lock so an abort-time repoll cannot turn
     * a cancelled late hit into a newly authorized command. */
    if(exited&&event==BARGE_WORKER_HIT&&(b->worker_stop||b->worker_cancel)) {
        b->pending=0;b->worker_status.last_decision="discarded";
        b->worker_status.state="idle";event=BARGE_WORKER_CANCELLED;
    }
    b->published_status=b->worker_status;b->published_candidate=b->candidate.valid;
    if(event)b->worker_event=event;if(exited)b->worker_exited=1;
    pthread_mutex_unlock(&b->worker_mutex);
}
static void barge_worker_pause(struct barge_monitor *b) {
    /* No mailbox lock is held while acquiring the reader FIFO lock. */
    pthread_mutex_lock(&b->capture.mutex);
    if(b->capture.fifo_used<CAPTURE_CHUNK&&!b->capture.reader_error) {
        struct timespec until;
#if defined(__APPLE__)
        until.tv_sec=0;until.tv_nsec=20000000;
        (void)pthread_cond_timedwait_relative_np(&b->capture.available,&b->capture.mutex,&until);
#else
        clock_gettime(CLOCK_MONOTONIC,&until);until.tv_nsec+=20000000;
        if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
        (void)pthread_cond_timedwait(&b->capture.available,&b->capture.mutex,&until);
#endif
    }
    pthread_mutex_unlock(&b->capture.mutex);
}
/* One real DSP step, shared by the worker and deterministic native algorithm
 * fixtures.0=continued,1=not-ready,2=confirmed hit,-2=discontinuity. It never
 * changes main UI/socket/ALSA ownership or joins/destroys its calling owner. */
static int barge_worker_step(struct barge_monitor *b) {
    pthread_mutex_lock(&b->capture.mutex);
    unsigned backlog=b->capture.fifo_peak;int transport_error=b->capture.reader_error;
    pthread_mutex_unlock(&b->capture.mutex);
    if(backlog>barge_status.capture_backlog_frames)barge_status.capture_backlog_frames=backlog;
    uint64_t consumed=b->capture.frames;int64_t begin=now_ms();
    int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES];
    unsigned feed_flags=barge_feed_flags(b);
    int result=!feed_flags||transport_error||(b->candidate.valid&&now_ms()>=b->candidate.wall_deadline)?CAPTURE_DISCONTINUITY:barge_next_clean(b,pcm,0);
    if(result==1) {
        if(consumed!=b->capture.frames)barge_dsp_duration(begin);
        wipe(pcm,sizeof(pcm));return 1;
    }
    if(!result)result=barge_reference_feed(b);
    if(!result)result=local_chain_feed(b->chain,pcm,b->frontend.selected,feed_flags,&b->frontend);
    if(!result)s12dbg_dsp(b->chain->direct_frames,b->chain->beam_frames,b->chain->paused_models,b->chain->afe!=NULL,&b->frontend);
    if(!result)s12dbg_wake_models(b->chain->diversity_wake,b->reference_wake,b->reference_model_epoch,0);
    if(!result&&b->chain->hit_source) {
        if(b->chain->hit_event.status!=1)result=-1;else barge_candidate_begin(b);
    }
    int accepted=!result?barge_candidate_decide(b):0;if(accepted<0)result=-1;
    barge_dsp_duration(begin);wipe(pcm,sizeof(pcm));
    if(result<0) {
        b->failed=CAPTURE_DISCONTINUITY;barge_candidate_clear(b,"discarded");
        barge_count(&barge_status.capture_discontinuities);barge_status.state="error";
        return CAPTURE_DISCONTINUITY;
    }
    barge_count(&barge_status.capture_blocks);
    if(accepted) {
        b->pending=1;barge_status.state="interrupted";
        return 2;
    }
    return 0;
}
static void *barge_worker_main(void *opaque) {
    struct barge_monitor *b=opaque;int event=BARGE_WORKER_NONE;
    pthread_mutex_lock(&barge_registry_mutex);
    barge_registry_thread=pthread_self();barge_registry_status=&b->worker_status;barge_registry_valid=1;
    pthread_mutex_unlock(&barge_registry_mutex);
    for(;;) {
        pthread_mutex_lock(&b->worker_mutex);
        int stop=b->worker_stop,eof=b->worker_eof;uint64_t eof_target=b->worker_eof_target;
        pthread_mutex_unlock(&b->worker_mutex);
        if(stop){barge_candidate_clear(b,"discarded");event=BARGE_WORKER_CANCELLED;break;}
        /* Flush the pre-EOF FIFO watermark before deciding that no already
         * captured candidate exists. A valid candidate retains its original
         * processed-frame lookahead and hard scheduling deadline. */
        if(eof&&!b->candidate.valid&&b->reference_frames>=(eof_target/CAPTURE_CHUNK)*6&&
           barge_model_progress(b)>=(eof_target/CAPTURE_CHUNK)*6&&b->clean_used<UPLINK_SAMPLES&&
           (b->monitor_source!=BARGE_SOURCE_BEAM||!b->chain->input_used)) {
            event=BARGE_WORKER_EOF;break;
        }
        int result=barge_worker_step(b);
        if(result==CAPTURE_DISCONTINUITY){event=BARGE_WORKER_FAILED;break;}
        if(result==2) {
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
            const char *pause=getenv("S12_TEST_WORKER_POST_HIT_PAUSE_MS");
            if(pause) {
                unsigned ms=(unsigned)strtoul(pause,NULL,10);if(ms>1000)ms=1000;
                pthread_mutex_lock(&b->worker_mutex);b->worker_test_after_hit=1;pthread_mutex_unlock(&b->worker_mutex);
                struct timespec delay={ms/1000,(ms%1000)*1000000};nanosleep(&delay,NULL);
            }
#endif
            event=BARGE_WORKER_HIT;break;
        }
        barge_worker_publish(b,0,0);
        if(result==1)barge_worker_pause(b);
    }
    /* Unregister before publishing exited: no global diagnostic operation or
     * vendor call follows that publication. Main may now immediately join. */
    pthread_mutex_lock(&barge_registry_mutex);barge_registry_valid=0;barge_registry_status=NULL;
    pthread_mutex_unlock(&barge_registry_mutex);
    barge_worker_publish(b,event,1);return NULL;
}
static int barge_worker_start(struct barge_monitor *b) {
    if(pthread_mutex_init(&b->worker_mutex,NULL))return -1;b->worker_mutex_ready=1;
    b->worker_status=barge_status;b->published_status=b->worker_status;
    if(pthread_create(&b->worker,NULL,barge_worker_main,b)) {
        pthread_mutex_destroy(&b->worker_mutex);b->worker_mutex_ready=0;return -1;
    }
    b->worker_started=1;return 0;
}
static void barge_worker_join(struct barge_monitor *b,int request_stop) {
    if(!b||!b->worker_started)return;
    if(request_stop) {
        pthread_mutex_lock(&b->worker_mutex);b->worker_stop=1;pthread_mutex_unlock(&b->worker_mutex);
        /* The worker waits at most20ms on the FIFO, not the mailbox. */
        pthread_mutex_lock(&b->capture.mutex);pthread_cond_broadcast(&b->capture.available);pthread_mutex_unlock(&b->capture.mutex);
    }
    if(pthread_join(b->worker,NULL)){(void)fail("barge worker join failed");_exit(1);}
    b->worker_started=0;
    /* request_stop cancels an event that main has not consumed, even if the
     * worker passed its last control check before stop reached the mailbox.
     * Accepted normal hits use join(request_stop=0) in barge_service. */
    if(request_stop&&b->pending){b->pending=0;b->worker_status.last_decision="discarded";}
    barge_merge_snapshot(&b->worker_status);
    pthread_mutex_destroy(&b->worker_mutex);b->worker_mutex_ready=0;
}
/* Main/UI thread only: short control/snapshot locks and a completed-thread
 * join. Never executes a vendor DSP call, consumes FIFO audio or reads rings. */
static void barge_service(void) {
    struct barge_monitor *b=barge_current;
    if(!b||!b->worker_started) {
        if(barge_config.enabled)barge_status_write(panel_enabled&&panel.muted?"muted":barge_status.state?barge_status.state:"idle",0);
        return;
    }
    int cancel=stopping||panel_interrupted||(panel_enabled&&panel.muted);
    int ota=ota_idle_requested();
    pthread_mutex_lock(&b->worker_mutex);
    if(cancel||ota)b->worker_cancel=1;if(cancel)b->worker_stop=1;
    struct barge_status_record snapshot=b->published_status;
    int exited=b->worker_exited,event=b->worker_event;
    int sticky_cancel=b->worker_stop||b->worker_cancel;
    pthread_mutex_unlock(&b->worker_mutex);
    cancel=cancel||sticky_cancel;
    barge_merge_snapshot(&snapshot);
    if(exited) {
        barge_worker_join(b,0);barge_active=0;
        if(event==BARGE_WORKER_HIT&&!cancel&&!ota) {
            barge_count(&barge_status.keyword_hits);
            panel_interrupted=1;local_wake_state("busy");panel_state("listening");
            barge_status_write("interrupted",1);
            printf("{\"event\":\"local_barge_detected\",\"source\":\"%s\",\"old_audio_retried\":false,\"input_frame\":%llu}\n",
                   b->chain->hit_source,(unsigned long long)b->chain->hit_event.input_frames);
        } else if(event==BARGE_WORKER_FAILED) {
            panel_interrupted=1;barge_failure="capture";barge_error_reason="capture_discontinuity";barge_status.last_error_reason=barge_error_reason;barge_status_write("error",1);
            puts("{\"event\":\"barge_capture_invalidated\",\"raw_kws_fallback\":false,\"audio_uploaded\":false}");
        } else {
            if(cancel||ota||event==BARGE_WORKER_CANCELLED) {
                b->pending=0;barge_status.last_decision="discarded";
                /* A cancelled worker can no longer drain capture. Release it
                 * now, including an OTA-inhibited turn that is still playing. */
                int closed=barge_stop(b);
                if(closed){b->failed=closed;panel_interrupted=1;barge_failure="capture";}
            }
            barge_status_write(panel_enabled&&panel.muted?"muted":"idle",1);
        }
    } else barge_status_write(panel_enabled&&panel.muted?"muted":"monitoring",0);
}
/* Main asks the worker to finish only after the pre-EOF capture watermark.
 * A captured pending candidate is still processed through its witness window. */
static void barge_confirm_tail(struct barge_monitor *b) {
    if(!b||!b->worker_started)return;
    pthread_mutex_lock(&b->capture.mutex);
    uint64_t watermark=b->capture.delivered_frames+b->capture.fifo_used;
    pthread_mutex_unlock(&b->capture.mutex);
    pthread_mutex_lock(&b->worker_mutex);b->worker_eof=1;b->worker_eof_target=watermark;
    pthread_mutex_unlock(&b->worker_mutex);
    while(b->worker_started&&!stopping&&!panel_interrupted) {
        panel_poll();
        if(b->worker_started){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    }
}
struct local_sentence {
    OpusEncoder *encoder;
    int16_t packet[UPLINK_SAMPLES];
    unsigned used,padding;
};
static int local_sentence_open(struct local_sentence *s) {
    int error=0;s->encoder=opus_encoder_create(16000,1,2048,&error);
    if(!s->encoder||error||opus_encoder_ctl(s->encoder,4002,24000)||opus_encoder_ctl(s->encoder,4010,4))
        return fail("local continuation encoder setup failed");
    return 0;
}
static int local_sentence_add(struct local_sentence *s,struct tts_queue *q,const int16_t *pcm) {
    if(q->count>=WAKE_MAX_PACKETS)return 1;
    memcpy(s->packet+s->used,pcm,320);s->used+=160;
    if(s->used==UPLINK_SAMPLES){if(encode_queued(s->encoder,q,s->packet))return -1;s->used=0;}
    return q->count>=WAKE_MAX_PACKETS?1:0;
}
static int local_sentence_finish(struct local_sentence *s,struct tts_queue *q) {
    if(!s->used)return 0;
    s->padding=UPLINK_SAMPLES-s->used;memset(s->packet+s->used,0,s->padding*2);
    return encode_queued(s->encoder,q,s->packet);
}
/* 1 wake-only/manual (caller gives local reply and a fresh 8s window),
 * LOCAL_STARTUP_PROBE: unfed real capture, closed before any startup cue,
 * 4 already completed same-sentence turn, 2 cancelled/OTA, -1 failure.
 * Capture remains the sole owner through keyword, grace and continuation.
 * The two rings use each persistent detector's verified 10ms frame IDs;
 * neither an ALSA frame count nor the other detector's clock indexes them. */
static int local_capture_turn(struct local_chain *chain,struct mic_frontend *frontend,unsigned min_rms,
                              struct tts_queue *q,struct barge_monitor *handoff) {
    struct capture owned;memset(&owned,0,sizeof(owned));
    struct capture *c=handoff?&handoff->capture:&owned;int rc=-1;
    struct local_sentence sentence;memset(&sentence,0,sizeof(sentence));
    struct vad idle_voice;vad_init(&idle_voice,min_rms);
    struct local_pcm_ring *ring=NULL;
    uint64_t scan=0,start=0,encoded=0,grace_end=0;
    int16_t onset_pcm[UPLINK_SAMPLES];unsigned onset_used=0,onset_blocks=0,silent=0;
    uint64_t onset_first=0;int announced=0,confirmed=0,input_ready=!!handoff;
    double threshold=min_rms;
    int64_t report=now_ms()+10000;
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES];
    if(!handoff) {
        mic_frontend_reopen(frontend);if(local_chain_reopen(chain))goto done;
        if(capture_open(c))goto done;
        int started=capture_start(c);
        if(started){if(started==CAPTURE_DISCONTINUITY)rc=LOCAL_CAPTURE_RETRY;goto done;}
    }
    if(handoff) {
        handoff->pending=0;barge_active=0;barge_status_write("handoff",0);
        puts("{\"event\":\"barge_capture_handoff\",\"capture_reopened\":false,\"keyword_clock_preserved\":true}");
    } else if(!startup_feedback.enabled) {
        local_capture_recovering=0;panel_state("idle");local_wake_state("ready");input_ready=1;
        printf("{\"event\":\"local_wake_listening\",\"local_keyword_spotting\":true,\"microphones\":6,\"frontend\":\"%s\",\"idle_audio_uploaded\":false}\n",chain->afe?"original_s12_beamformer":"six_channel_selection");
    } else {
        local_wake_state(local_capture_recovering?"recovering":"starting");
        if(startup_feedback.pending||startup_feedback.ready_after_capture)panel_state("boot");
    }
    while(!stopping) {
        panel_poll();
        if(panel_interrupted||(panel_enabled&&panel.muted)){rc=2;break;}
        if(!announced&&ota_idle_requested()){rc=2;break;}
        if(!announced&&panel_enabled&&panel.manual!=panel_manual_used){panel_manual_used=panel.manual;rc=1;break;}
        c->deadline=now_ms()+1500;
        int read_result=handoff?barge_next_clean(handoff,pcm,1):capture_exact(c,raw,CAPTURE_CHUNK);
        if(read_result){if(panel_interrupted)rc=2;else if(read_result==CAPTURE_DISCONTINUITY)rc=LOCAL_CAPTURE_RETRY;goto done;}
        if(startup_feedback.enabled&&!input_ready) {
            if(startup_feedback.pending) {
                if(chain->wake&&chain->diversity_wake&&chain->afe){rc=LOCAL_STARTUP_PROBE;goto done;}
                startup_feedback.pending=0;
                puts("{\"event\":\"startup_feedback_disabled\",\"reason\":\"frontend_unavailable\"}");
            }
            input_ready=1;local_capture_recovering=0;local_wake_state("ready");panel_state("idle");
            printf("{\"event\":\"local_wake_listening\",\"local_keyword_spotting\":true,\"microphones\":6,\"frontend\":\"%s\",\"idle_audio_uploaded\":false}\n",chain->afe?"original_s12_beamformer":"six_channel_selection");
        }
        if(!announced)startup_idle_ui();
        if(!handoff&&mic_frontend_process(frontend,raw,CAPTURE_CHUNK,min_rms,pcm)!=UPLINK_SAMPLES){fail("local wake resampling count mismatch");goto done;}
        unsigned feed_flags=handoff?barge_feed_flags(handoff):LOCAL_FEED_KWS|LOCAL_FEED_RECORD;
        if(!feed_flags||local_chain_feed(chain,pcm,frontend->selected,feed_flags,frontend)<0)goto done;
        s12dbg_dsp(chain->direct_frames,chain->beam_frames,chain->paused_models,chain->afe!=NULL,frontend);
        if(!announced&&!chain->hit_source&&input_ready) {
            idle_voice.noise=frontend->noise[frontend->selected];
            if(vad_step(&idle_voice,pcm[frontend->selected])==2)vad_relisten(&idle_voice);
            /* Button feedback waits until no local speech is active. */
            panel_poll();
            if(!idle_voice.active&&!idle_voice.streak&&ui_feedback_ready()){rc=LOCAL_UI_FEEDBACK;break;}
        }
        if(!announced&&chain->hit_source) {
            const struct s12_vendor_wake_event *e=&chain->hit_event;
            ring=!strcmp(chain->hit_source,"six_channel_selection")?&chain->direct_ring:&chain->beam_ring;
            frontend->locked=1;local_wake_state("busy");panel_state("listening");announced=1;
            printf("{\"event\":\"local_wake_detected\",\"network_required\":false,\"source\":\"%s\",\"input_frame\":%llu,\"begin_frame\":%llu,\"end_frame\":%llu}\n",
                   chain->hit_source,(unsigned long long)e->input_frames,(unsigned long long)e->begin_frame,(unsigned long long)e->end_frame);
            /* Missing/old timing never authorizes arbitrary idle preroll.
             * Fall back to the established local reply + fresh recording. */
            uint64_t first=e->begin_frame?e->begin_frame:1;
            if(!e->timing_valid||e->status!=1||e->begin_frame>=e->end_frame||e->end_frame>e->input_frames||
               e->end_frame>UINT64_MAX-LOCAL_CONTINUATION_GRACE||
               e->input_frames>UINT64_MAX-LOCAL_CONTINUATION_GRACE-LOCAL_CONTINUATION_ONSET||
               !local_ring_get(ring,first)||!local_ring_get(ring,e->end_frame)) {
                s12dbg_fallback(1);puts("{\"event\":\"local_timing_fallback\",\"audio_uploaded\":false}");rc=1;break;
            }
            if(ring==&chain->direct_ring) {
                /* Emit can lag the word by hundreds of milliseconds. Use
                 * the microphone selected during that confirmed word, not
                 * a new idle/noise winner at the later emit instant. All six
                 * FIR streams were retained at identical frame IDs. */
                uint64_t middle=e->begin_frame+(e->end_frame-e->begin_frame)/2;
                int index=local_ring_index(ring,middle);
                if(index<0){rc=1;break;}
                chain->hit_channel=chain->direct_selected[index];frontend->selected=chain->hit_channel;
            }
            start=e->begin_frame>8?e->begin_frame-8:1;
            if(!local_ring_get(ring,start))start=first;
            /* The SDK's model alignment can end270ms before the real word
             * tail (verified original-model wake-only replay). Its end is
             * not a phonetic end-of-speech marker. Require a conservative
             * post-emit continuation, never count the final 学 as a query. */
            uint64_t anchor=e->end_frame+LOCAL_WORD_TAIL_GUARD;
            if(anchor<e->input_frames+LOCAL_EMIT_EDGE_GUARD)anchor=e->input_frames+LOCAL_EMIT_EDGE_GUARD;
            scan=anchor+1;grace_end=e->input_frames+LOCAL_CONTINUATION_GRACE;
            if(grace_end<scan+LOCAL_CONTINUATION_ONSET-1)grace_end=scan+LOCAL_CONTINUATION_ONSET-1;
            threshold=frontend->noise[frontend->selected]*3.5;
            if(ring==&chain->beam_ring) {
                if(!chain->beam_noise_ready){s12dbg_fallback(2);puts("{\"event\":\"local_timing_fallback\",\"reason\":\"beam_noise_unavailable\",\"audio_uploaded\":false}");rc=1;break;}
                threshold=chain->beam_noise*3.5;
            }
            if(threshold<min_rms)threshold=min_rms;
            s12dbg_cont_window(chain->hit_source,frontend->selected,e->begin_frame,e->end_frame,e->input_frames,start,anchor,scan,grace_end,local_ring_latest(ring),ring->count,threshold,ring==&chain->beam_ring?chain->beam_noise:frontend->noise[frontend->selected]);
        }
        if(announced) {
            uint64_t latest=local_ring_latest(ring);
            for(;scan<=latest;++scan) {
                const int16_t *block=local_chain_saved(chain,ring,scan);
                if(!block){fail("local continuation ring boundary unavailable");goto done;}
                int voiced=mic_rms(block,160)>=threshold;
                if(!confirmed) {
                    if(voiced&&!onset_first)onset_first=scan;
                    memcpy(onset_pcm+onset_used,block,320);onset_used+=160;
                    if(onset_used==UPLINK_SAMPLES) {
                        if(mic_rms(onset_pcm,UPLINK_SAMPLES)>=threshold)++onset_blocks;
                        else {onset_blocks=0;onset_first=0;}
                        s12dbg_cont_probe(mic_rms(onset_pcm,UPLINK_SAMPLES),onset_blocks);
                        onset_used=0;
                    }
                    if(onset_blocks>=WAKE_SPEECH_PACKETS) {
                        /* A long model word plus post-emit safety guard can
                         * outlive this bounded ring after the initial hit.
                         * Revalidate before copying any prefix; an expired
                         * range requests a fresh command, not a fatal exit. */
                        if(latest<start||latest-start>=LOCAL_RING_FRAMES||!local_ring_get(ring,start)) {
                            s12dbg_fallback(3);puts("{\"event\":\"local_timing_fallback\",\"reason\":\"preroll_expired\",\"audio_uploaded\":false}");rc=1;goto done;
                        }
                        if(local_sentence_open(&sentence))goto done;
                        /* Copy the complete confirmed keyword and its tail,
                         * including audio spoken before the hit was emitted.
                         * Source/time axis is locked for this whole sentence. */
                        for(uint64_t id=start;id<=latest;++id) {
                            const int16_t *saved=local_chain_saved(chain,ring,id);
                            if(!saved||local_sentence_add(&sentence,q,saved)<0){fail("local continuation preroll unavailable");goto done;}
                        }
                        encoded=latest;confirmed=1;silent=0;panel_state("capturing");
                        s12dbg_cont_end(1,start,scan,grace_end,latest,onset_first,onset_blocks,threshold);
                        printf("{\"event\":\"local_continuation_started\",\"source\":\"%s\",\"keyword_retained\":true,\"first_frame\":%llu,\"encoded_until\":%llu,\"capture_reopened\":false}\n",
                               chain->hit_source,(unsigned long long)start,(unsigned long long)encoded);
                    }
                } else {
                    silent=voiced?0:silent+1;
                    if(scan>encoded){int r=local_sentence_add(&sentence,q,block);if(r<0)goto done;encoded=scan;if(r){rc=4;goto finish;}}
                    if(silent>=WAKE_SILENCE_PACKETS*6){rc=4;goto finish;}
                }
            }
            if(!confirmed&&latest>=grace_end) {
                /* A word that begins at the grace boundary still gets its
                 * bounded180ms onset confirmation; do not cut its first
                 * partial packet merely because the grace expired mid-packet. */
                int pending=onset_first&&onset_first<=grace_end+1&&latest-grace_end<LOCAL_CONTINUATION_ONSET;
                if(!pending){s12dbg_cont_end(4,start,scan,grace_end,latest,onset_first,onset_blocks,threshold);s12dbg_fallback(4);rc=1;break;}
            }
        } else if(now_ms()>=report){mic_frontend_report(frontend,"local_wake_idle");report=now_ms()+10000;}
    }
    goto done;
finish:;
    int closed=(handoff?barge_stop(handoff):capture_finish(c));
    if(closed){rc=closed==CAPTURE_DISCONTINUITY?LOCAL_CAPTURE_RETRY:-1;goto done;}
    if(local_sentence_finish(&sentence,q)){rc=-1;goto done;}
    printf("{\"event\":\"utterance_complete\",\"same_sentence\":true,\"audio_packets\":%u,\"duration_ms\":%u,\"compressed_bytes\":%llu,\"padding_samples\":%u,\"max_duration_reached\":%s,\"short_reads_assembled\":%u}\n",
           q->count,q->count*60,(unsigned long long)q->used,sentence.padding,q->count>=WAKE_MAX_PACKETS?"true":"false",c->short_reads);
done:
    {int close_result=(handoff?barge_stop(handoff):capture_finish(c));if(close_result&&!stopping)rc=close_result==CAPTURE_DISCONTINUITY?LOCAL_CAPTURE_RETRY:-1;}
    startup_ready_clear();
    wipe(raw,sizeof(raw));wipe(pcm,sizeof(pcm));
    wipe(onset_pcm,sizeof(onset_pcm));
    if(sentence.encoder)opus_encoder_destroy(sentence.encoder);wipe(&sentence,sizeof(sentence));
    wipe(&chain->direct_ring,sizeof(chain->direct_ring));wipe(&chain->beam_ring,sizeof(chain->beam_ring));
    wipe(chain->direct_channels,sizeof(chain->direct_channels));wipe(chain->direct_selected,sizeof(chain->direct_selected));
    if(rc!=4||stopping){wipe(q->bytes,q->used);q->used=0;q->count=0;}
    if(rc==LOCAL_CAPTURE_RETRY)local_wake_state("recovering");
    else if(rc<0&&!stopping)local_wake_state("error");else if(rc==1||rc==4)local_wake_state("busy");
    puts("{\"event\":\"local_capture_closed\"}");
    return stopping?0:rc;
}
/* Ordinary recordings close before a WebSocket upload. A followup ready cue
 * instead prewarms one reader, discards its cue interval, then keeps that same
 * reader and raw preroll for immediate speech. The verified barge monitor
 * separately owns capture during a remote
 * turn and hands that same owner to a confirmed continuation. A small raw
 * preroll survives VAD onset; only the completed bounded utterance
 * is kept as Opus in RAM. No PCM/compressed audio file is created. */
/* Both modes retain a single capture owner. Local mode passes the six FIR
 * outputs through the original AFE before VAD/Opus, retaining an aligned
 * independent microphone FIFO for the diversity fallback. */
/* A short readiness chirp can leave a tonal room tail. During only the first
 *480ms, reject an onset only after three consecutive packets are dominated by
 *the known500..600Hz cue band and decay by at least10% per packet. Speech,
 *steady tones and other bands retain every preroll sample. This replaces no
 *time-based cold guard and never maps an ASR result to a command. */
struct ready_echo {unsigned tonal;double last_rms,last_frequency;};
static double ready_tone_fraction(const int16_t *pcm,double mean,double power,double frequency) {
    double coefficient=2*cos(2*PI*frequency/16000),q1=0,q2=0;
    for(unsigned i=0;i<UPLINK_SAMPLES;++i){double q0=pcm[i]-mean+coefficient*q1-q2;q2=q1;q1=q0;}
    return 2*(q1*q1+q2*q2-coefficient*q1*q2)/(UPLINK_SAMPLES*power);
}
static int ready_echo_only(struct ready_echo *echo,const int16_t *pcm,double rms) {
    static const double frequency[]={500,525,550,575,600};
    double mean=0,power=0,best=0,selected=0;for(unsigned i=0;i<UPLINK_SAMPLES;++i)mean+=pcm[i];mean/=UPLINK_SAMPLES;
    for(unsigned i=0;i<UPLINK_SAMPLES;++i){double x=pcm[i]-mean;power+=x*x;}
    if(rms<2||power<1){echo->tonal=0;echo->last_rms=rms;return 0;}
    for(unsigned f=0;f<sizeof(frequency)/sizeof(*frequency);++f) {
        double fraction=ready_tone_fraction(pcm,mean,power,frequency[f]);if(fraction>best){best=fraction;selected=frequency[f];}
    }
    /* A voiced fundamental near550Hz is not the cue: retain harmonic speech
     * and changing pitch. The microphone preroll is never cleared here. */
    int harmonic=best>=0.55&&(ready_tone_fraction(pcm,mean,power,selected*2)>=0.02||ready_tone_fraction(pcm,mean,power,selected*3)>=0.02);
    /* The first packet may include the chirp's attack/filter transient. It
     * merely seeds a candidate; two later packets must be>=80% pure tone. */
    if(best>=0.55&&!harmonic)echo->tonal=echo->tonal&&best>=0.80&&selected==echo->last_frequency&&rms<=echo->last_rms*0.90?echo->tonal+1:1;
    else echo->tonal=0;
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    if(getenv("S12_TEST_READY_ECHO_DEBUG"))printf("{\"event\":\"ready_echo_probe\",\"rms\":%.2f,\"fraction\":%.3f,\"frequency\":%.0f,\"harmonic\":%d,\"streak\":%u}\n",rms,best,selected,harmonic,echo->tonal);
#endif
    echo->last_rms=rms;echo->last_frequency=selected;return echo->tonal>=WAKE_SPEECH_PACKETS;
}
/* Only an already-authorized six-mic command capture uses these independent
 * energy witnesses. No idle KWS or auxiliary speech classifier is involved.
 * A source must provide its own consecutive180ms, never another source's tail. */
struct command_onset {
    struct vad energy;
    struct ready_echo echo;
    double recent[3];
    unsigned next;
};
static int command_onset_step(struct command_onset *source,const int16_t *pcm,
                               double noise,int healthy,unsigned min_rms,
                               int cold,int guard_echo,int *echo_ignored) {
    if(!healthy) {
        vad_relisten(&source->energy);wipe(&source->echo,sizeof(source->echo));
        return 0;
    }
    source->energy.min_rms=min_rms;source->energy.noise=noise;
    int hit=vad_step(&source->energy,pcm);
    source->recent[source->next]=source->energy.last_rms;source->next=(source->next+1)%3;
    if(guard_echo&&ready_echo_only(&source->echo,pcm,source->energy.last_rms)) {
        vad_relisten(&source->energy);*echo_ignored=1;return 0;
    }
    if(cold){vad_relisten(&source->energy);return 0;}
    return hit==1;
}
static int command_raw_healthy(const int16_t *pcm,int current_healthy) {
    if(!current_healthy)return 0;
    /* The AFE's alignment queue can retain a different raw packet from the
     * newest health metrics. Both ends must be good; 98.5%/0.5% matches the
     * existing physical-mic guard. A hard failure clears only this witness. */
    unsigned clipped=0;for(unsigned i=0;i<UPLINK_SAMPLES;++i)if(pcm[i]>32275||pcm[i]<-32275)++clipped;
    return clipped<=(UPLINK_SAMPLES*5u)/1000u;
}
static int followup_prepare_gate(void) {
    panel_poll();
    if(stopping||panel_interrupted||ota_idle_requested()||
       (panel_enabled&&(panel.muted||panel.manual!=panel_manual_used)))return 2;
    if(followup_capture_deadline&&!remaining(followup_capture_deadline))return 3;
    if(followup_capture_deadline&&session_keepalive.failed)return FOLLOWUP_SESSION_CLOSED;
    return 0;
}
static int followup_prepare_input(struct capture *c,struct vad *v,struct mic_frontend *frontend,
                                  struct local_chain *chain,unsigned wait_ms,const struct audio *options,int *speech_failed) {
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES];int rc=-1;
    panel_state("thinking");
    /* Consume at least the former180ms guard before any ready sound. A fresh
     * AFE needs current-epoch quiet output for its independent floor. Stop as
     * soon as that is verified, or safely use raw after at most480ms. Only
     * pre-cue ambient calibration reaches DSP; the ready boundary is unchanged. */
    unsigned warm_packets=0;
    while(warm_packets<8) {
        int gate=followup_prepare_gate();if(gate){rc=gate;goto done;}
        c->deadline=now_ms()+1500;
        int got=capture_exact(c,raw,CAPTURE_CHUNK);gate=followup_prepare_gate();if(gate){rc=gate;goto done;}if(got){rc=got;goto done;}
        if(mic_frontend_process(frontend,raw,CAPTURE_CHUNK,(unsigned)v->min_rms,pcm)!=UPLINK_SAMPLES){fail("followup warmup sample count mismatch");goto done;}
        if(chain) {
            if(local_chain_feed(chain,pcm,frontend->selected,LOCAL_FEED_KWS|LOCAL_FEED_QUEUE,frontend)<0)goto done;
            while(chain->output_used>=UPLINK_SAMPLES&&chain->raw_used>=UPLINK_SAMPLES) {
                if(local_wake_enabled&&!*speech_failed&&command_speech_frames(v,chain->afe&&chain->beam_noise_ready?chain->output:chain->raw[frontend->selected])<0)*speech_failed=1;
                local_chain_consume(chain);
            }
        }
        ++warm_packets;
        if(warm_packets>=WAKE_SPEECH_PACKETS&&(!chain||!chain->afe||chain->beam_noise_ready))break;
    }
    pthread_mutex_lock(&c->mutex);
    c->reader_prompt_drop=1;c->reader_prompt_quiesced=0;c->reader_prompt_ready=0;
    c->prompt_discarded_frames+=c->fifo_used;c->delivered_frames+=c->fifo_used;
    c->fifo_read=c->fifo_write;c->fifo_used=0;
    pthread_mutex_unlock(&c->mutex);
    /* Observe the reader entering tiny cue reads before playing: an older
     * normal1024-frame read must not span both cue boundaries unnoticed. */
    int64_t quiet_deadline=now_ms()+1500;
    for(;;) {
        int gate=followup_prepare_gate();if(gate){rc=gate;goto done;}
        pthread_mutex_lock(&c->mutex);int quiet=c->reader_prompt_quiesced,error=c->reader_error;pthread_mutex_unlock(&c->mutex);
        if(error){rc=capture_discontinuity(error,"followup_prepare");if(!rc)rc=-1;goto done;}
        if(quiet)break;
        if(!remaining(quiet_deadline)){fail("followup reader cue boundary timed out");goto done;}
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    printf("{\"event\":\"followup_input_prepared\",\"warmup_ms\":%u,\"single_reader\":true,\"audio_uploaded\":false}\n",warm_packets*60);
    command_feedback_active=1;int played=local_prompt("followup",options);command_feedback_active=0;
    int gate=followup_prepare_gate();if(gate){rc=gate;goto done;}if(played)goto done;
    pthread_mutex_lock(&c->mutex);
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    c->fake_time_origin=c->source_frames;c->started=now_ms();c->fake_ready_phase=1;
#endif
    c->reader_prompt_resume=1;c->reader_prompt_drop=0;pthread_cond_broadcast(&c->available);pthread_mutex_unlock(&c->mutex);
    int64_t ready_deadline=now_ms()+1500;
    for(;;) {
        gate=followup_prepare_gate();if(gate){rc=gate;goto done;}
        pthread_mutex_lock(&c->mutex);int ready=c->reader_prompt_ready,error=c->reader_error;pthread_mutex_unlock(&c->mutex);
        if(error){rc=capture_discontinuity(error,"followup_ready");if(!rc)rc=-1;goto done;}
        if(ready)break;
        if(!remaining(ready_deadline)){fail("followup input readiness timed out");goto done;}
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    /* Remove only pre-cue DSP queue fragments; retain the calibrated frontend
     * and AFE instance. From this point every physical input sample is retained. */
    if(chain) {
        wipe(chain->input,sizeof(chain->input));wipe(chain->output,sizeof(chain->output));wipe(chain->raw,sizeof(chain->raw));
        chain->input_used=chain->output_used=chain->raw_used=0;
    }
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    if(getenv("S12_TEST_READY_FORCE_BOUNDARY"))printf("{\"event\":\"followup_eof_boundary_fixture\",\"inflight_read_forced\":%s,\"retained_frames\":%u,\"first_source_frame\":%llu}\n",c->fake_boundary_wait?"true":"false",c->fake_boundary_retained,(unsigned long long)c->fake_boundary_first_frame);
#endif
    gate=followup_prepare_gate();if(gate){rc=gate;goto done;}
    vad_relisten(v);
    if(chain)v->noise=frontend->noise[frontend->selected];
    printf("{\"event\":\"followup_listening\",\"wake_prompt_replayed\":false,\"followup_cue_completed\":true,\"input_already_ready\":true,\"window_ms\":%u,\"empty_audio_uploaded\":false}\n",wait_ms);
    barge_count(&barge_status.followup_windows);barge_status.conversation_active=1;barge_status_write("followup",1);panel_state("listening");
    rc=0;
done:
    command_feedback_active=0;
    if(rc<0&&(stopping||panel_interrupted||ota_idle_requested()||
       (panel_enabled&&(panel.muted||panel.manual!=panel_manual_used))))rc=2;
    if(followup_capture_deadline&&session_keepalive.failed)rc=FOLLOWUP_SESSION_CLOSED;
    wipe(raw,sizeof(raw));wipe(pcm,sizeof(pcm));return rc;
}
static int capture_utterance(struct vad *v,struct mic_frontend *frontend,struct tts_queue *q,
                             struct local_chain *chain,unsigned wait_ms,const struct audio *options) {
    int ready_prompt=followup_prompt_pending,quiet_relisten=followup_quiet_pending;followup_prompt_pending=followup_quiet_pending=0;
    memset(&q->capture_stats,0,sizeof(q->capture_stats));
    int use_speech=local_wake_enabled&&chain!=NULL,speech_failed=0;
    unsigned speech_silence=0,endpoint_silence=0,speech_seen=0,reference_at=0,reference_used=0;
    double recent_rms[3]={0},speech_reference=0;
    struct capture c;memset(&c,0,sizeof(c));
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    c.fake_command=chain!=NULL;c.fake_ready_requested=ready_prompt;
#endif
    mic_frontend_reopen(frontend);vad_relisten(v);
    int reopen_failed=0;
    OpusEncoder *encoder=NULL;int err=0,rc=-1;
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES];
    int16_t preroll[MIC_CHANNELS+1][LOCAL_COLD_PREROLL_PACKETS][UPLINK_SAMPLES];
    unsigned cold_packets=chain&&!ready_prompt&&!quiet_relisten?WAKE_SPEECH_PACKETS:0;
    unsigned preroll_packets=chain?LOCAL_COLD_PREROLL_PACKETS:WAKE_PREROLL_PACKETS;
    unsigned command_blocks=0;struct ready_echo echo={0};
    struct command_onset onsets[MIC_CHANNELS+1]={0};
    unsigned next=0,kept=0;int64_t idle_report=now_ms()+10000;
    double idle_peak=0;
    if(ready_prompt||quiet_relisten)panel_next_poll=0; /* Refresh controls before authorizing a new ready reader. */
    panel_poll();
    if(ota_idle_requested()){(void)ota_idle_wait();rc=2;goto out;}
    if(stopping||panel_interrupted||(panel_enabled&&(panel.muted||panel.manual!=panel_manual_used))){rc=2;goto out;}
    if(quiet_relisten&&(!followup_capture_deadline||!followup_onset_deadline||!remaining(followup_capture_deadline)||!remaining(followup_onset_deadline))){rc=3;goto out;}
    reopen_failed=chain?local_chain_reopen(chain):0;if(reopen_failed)goto out;
    if(capture_open(&c))goto out;
    int started=capture_start(&c);
    if(started){if(chain&&started==CAPTURE_DISCONTINUITY)rc=LOCAL_CAPTURE_RETRY;goto out;}
    if(chain)local_capture_recovering=0;
    if(ready_prompt) {
        int prepared=followup_prepare_input(&c,v,frontend,chain,wait_ms,options,&speech_failed);if(prepared){rc=prepared;goto out;}
    }
    uint64_t input_start_frames=c.frames;
    /* The PCM FIFO can contain a word captured before the eight-second
     * onset limit while an inference is briefly busy. Consume that physical
     * input range; wall time is only a bounded emergency limit including the
     * two-second FIFO, never a reason to discard already captured speech. */
    if(followup_capture_deadline&&wait_ms) {
        if(!followup_onset_deadline)followup_onset_deadline=now_ms()+wait_ms;
        int left=remaining(followup_onset_deadline);wait_ms=left?(unsigned)left:1;
    }
    if(quiet_relisten)printf("{\"event\":\"quiet_followup_listening\",\"window_ms\":%u,\"new_cue\":false,\"deadline_reset\":false,\"empty_audio_uploaded\":false}\n",wait_ms);
    int64_t wait_deadline=wait_ms?now_ms()+wait_ms+2000:0;
    panel_state(chain?"capturing":"listening");
    printf("{\"event\":\"wake_listening\",\"mic_channel\":%u,\"mic_mode\":\"%s\",\"noise_rms\":%.1f,\"local_keyword_spotting\":%s}\n",
           frontend->selected,frontend->mode==MIC_AUTO?"auto":"fixed",v->noise,chain?"true":"false");
    if(chain)printf("{\"event\":\"local_bf_noise_prior\",\"verified\":%s,\"noise_rms\":%.2f,\"cold_guard_ms\":%u}\n",
                    chain->beam_noise_ready?"true":"false",chain->beam_noise,cold_packets*60);
    while(!stopping) {
        if(followup_capture_deadline&&session_keepalive.failed){rc=FOLLOWUP_SESSION_CLOSED;break;}
        if(followup_capture_deadline&&!remaining(followup_capture_deadline)){rc=3;break;}
        if(!encoder&&wait_ms&&(now_ms()>=wait_deadline||c.frames-input_start_frames>=(uint64_t)wait_ms*48)) {
            puts("{\"event\":\"local_command_timeout\",\"audio_uploaded\":false}");rc=3;break;
        }
        if(!encoder&&ota_idle_requested()) {
            capture_close(&c);
            if(followup_capture_deadline&&session_keepalive.wire)session_wire_close(session_keepalive.wire,1);
            (void)ota_idle_wait();rc=2;goto out;
        }
        if(!encoder&&panel_enabled&&panel.manual!=panel_manual_used){rc=2;break;}
        if(!encoder&&!v->active&&!v->streak&&ui_feedback_ready()){rc=LOCAL_UI_FEEDBACK;break;}
        /* Empty ALSA input cannot stall a persistent idle loop forever. Legal
         * positive short reads are assembled; XRUN always rejects the turn. */
        c.deadline=now_ms()+1500;
        int read_result=capture_exact(&c,raw,CAPTURE_CHUNK);
        if(read_result){if(followup_capture_deadline&&session_keepalive.failed)rc=FOLLOWUP_SESSION_CLOSED;else if(chain&&read_result==CAPTURE_DISCONTINUITY)rc=LOCAL_CAPTURE_RETRY;goto out;}
#if defined(XIAOZHI_TEST_FAKE_CAPTURE) && defined(S12_TEST_READY_RAW_CHECK)
        if(ready_prompt)S12_TEST_READY_RAW_CHECK(raw,CAPTURE_CHUNK,c.fake_time_origin,c.frames-input_start_frames-CAPTURE_CHUNK);
#endif
        unsigned previous=frontend->selected;
        int independent_onset=chain&&frontend->mode==MIC_AUTO&&!encoder;
        int onset_source=-1,source_echo_ignored=0;unsigned onset_streak=0;
        if(mic_frontend_process(frontend,raw,CAPTURE_CHUNK,(unsigned)v->min_rms,pcm)!=UPLINK_SAMPLES){fail("utterance resampling count mismatch");goto out;}
        int16_t *mono=pcm[frontend->selected];
        if(chain) {
            /* Real capture remains continuous for the original KWS clocks,
             * including the quiet eight-second onset window. A command's
             * new keyword hits are ignored: only its authorized VAD turn
             * controls UI/network, never another wake event. Each BF output
             * is processed once and then queued by the same owner. */
            if(local_chain_feed(chain,pcm,frontend->selected,LOCAL_FEED_KWS|LOCAL_FEED_QUEUE,frontend)<0)goto out;
            s12dbg_dsp(chain->direct_frames,chain->beam_frames,chain->paused_models,chain->afe!=NULL,frontend);
            if(chain->output_used<UPLINK_SAMPLES)continue;
            if(chain->raw_used<UPLINK_SAMPLES){fail("microphone alignment queue incomplete");goto out;}
            /* Keep corresponding direct PCM until the BF's 128->160 frame
             * combiner produces a whole packet. Never discard the first raw
             * packet or compare a delayed beam with a newer microphone. */
            for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(pcm[ch],chain->raw[ch],UPLINK_SAMPLES*2);
            mono=chain->output;
            double original=mic_rms(pcm[frontend->selected],UPLINK_SAMPLES),beam=mic_rms(mono,UPLINK_SAMPLES);
            unsigned clipped=0;for(unsigned i=0;i<UPLINK_SAMPLES;++i)if(mono[i]>=32700||mono[i]<=-32700)++clipped;
            int beam_healthy=chain->afe&&clipped<=UPLINK_SAMPLES/100&&beam>=1&&
                (original<(double)v->min_rms||beam>=original*0.15);
            int previous_mode=chain->command_mode;
            if(!frontend->locked)chain->command_mode=beam_healthy&&chain->beam_noise_ready?1:0;
            if(independent_onset) {
                int cold=command_blocks<cold_packets,guard_echo=ready_prompt&&command_blocks<8;
                double best_score=0;
                /* A complete BF witness retains priority in a near tie. Only
                 * an independently confirmed raw source with clearly better
                 * own-domain SNR can replace it, never a pending witness. */
                if(command_onset_step(&onsets[MIC_CHANNELS],chain->output,chain->beam_noise,
                        beam_healthy&&chain->beam_noise_ready,(unsigned)v->min_rms,cold,guard_echo,&source_echo_ignored)) {
                    onset_source=MIC_CHANNELS;double floor=chain->beam_noise;
                    if(floor<v->min_rms/3.5)floor=v->min_rms/3.5;
                    best_score=onsets[MIC_CHANNELS].energy.last_rms/floor;
                }
                for(unsigned ch=0;ch<MIC_CHANNELS;++ch) {
                    int healthy=command_raw_healthy(pcm[ch],frontend->healthy[ch]);
                    if(command_onset_step(&onsets[ch],pcm[ch],frontend->noise[ch],healthy,
                            (unsigned)v->min_rms,cold,guard_echo,&source_echo_ignored)) {
                        double floor=frontend->noise[ch];if(floor<v->min_rms/3.5)floor=v->min_rms/3.5;
                        double score=onsets[ch].energy.last_rms/floor;
                        if(onset_source<0||score>best_score*(onset_source==MIC_CHANNELS?1.35:1)) {
                            onset_source=(int)ch;best_score=score;
                        }
                    }
                }
                for(unsigned source=0;source<=MIC_CHANNELS;++source)
                    if(onsets[source].energy.streak>onset_streak)onset_streak=onsets[source].energy.streak;
                if(onset_source>=0) {
                    chain->command_mode=onset_source==MIC_CHANNELS;
                    if(!chain->command_mode)frontend->selected=(unsigned)onset_source;
                }
                /* Keep the original classifier instance. Only an actual
                 * source witness authorizes the main energy VAD's third step. */
                vad_relisten(v);if(onset_source>=0)v->streak=WAKE_SPEECH_PACKETS-1;
            } else if(!encoder&&(previous_mode!=chain->command_mode||(!chain->command_mode&&previous!=frontend->selected)))vad_relisten(v);
            if(!chain->command_mode)mono=pcm[frontend->selected];
            v->noise=chain->command_mode?chain->beam_noise:frontend->noise[frontend->selected];
        }
        if(frontend->mode==MIC_AUTO&&!chain) {
            v->noise=frontend->noise[frontend->selected];
            if(frontend->selected!=previous)mic_frontend_report(frontend,"selection");
        }
        int speech_votes=0;
        if(use_speech&&!speech_failed){speech_votes=command_speech_frames(v,mono);if(speech_votes<0){speech_failed=1;speech_votes=0;speech_silence=endpoint_silence=0;q->capture_stats.auxiliary_valid=0;q->capture_stats.speech_frames=q->capture_stats.vad_frames=q->capture_stats.max_silence_ms=0;}}
        /* An already-active command can fall below its learned energy
         * threshold while real speech continues. Confirm current speech
         * before resetting only the energy-silence streak; onset, the
         * auxiliary endpoint and the twelve-second cap remain unchanged. */
        if(use_speech&&!speech_failed&&v->active&&speech_votes>=3&&
           frame_rms(mono,UPLINK_SAMPLES)>=v->min_rms)v->silence=0;
        int previously_active=v->active;int event=vad_step(v,mono);
        if(independent_onset&&onset_source<0){event=0;v->active=0;v->streak=onset_streak;}
        if(use_speech&&!speech_failed) {
            /* Preserve sensitive energy onset. A real speech classifier only
             * assists ending, with extra hangover and a conservative energy
             * drop guard. Unknown/high-energy low-SNR speech keeps the legacy
             * endpoint and original PCM for remote ASR; no noise veto. */
            if(previously_active) {
                speech_silence=speech_votes?0:speech_silence+1;
                if(speech_votes){speech_seen=1;endpoint_silence=0;}
                else if(speech_seen&&speech_reference>0&&v->last_rms<=speech_reference*.5)++endpoint_silence;
                else endpoint_silence=0;
                event=v->silence>=WAKE_SILENCE_PACKETS||endpoint_silence>=COMMAND_SPEECH_SILENCE_PACKETS?2:0;
            } else {speech_silence=endpoint_silence=0;}
            recent_rms[reference_at]=v->last_rms;reference_at=(reference_at+1)%3;if(reference_used<3)++reference_used;
            if(event==1&&reference_used==3){speech_reference=middle_rms(recent_rms[0],recent_rms[1],recent_rms[2]);speech_seen=speech_votes>0;}
            else if(previously_active&&speech_votes&&reference_used==3) {
                double reference=middle_rms(recent_rms[0],recent_rms[1],recent_rms[2]);if(reference>speech_reference)speech_reference=reference;
            }
        }
        if(independent_onset&&onset_source>=0) {
            /* Endpoint energy reference must use the winner's actual three
             * packets, not the provisional BF/mic stream used for warmup. */
            memcpy(recent_rms,onsets[onset_source].recent,sizeof(recent_rms));
            reference_at=onsets[onset_source].next;reference_used=3;
            speech_reference=middle_rms(recent_rms[0],recent_rms[1],recent_rms[2]);speech_seen=speech_votes>0;
        }
        /* Cue reverberation can decay across three packets and impersonate
         * an onset, including after a new AFE opens with an ambient prior.
         * Retain every sample, but do not accumulate those first180ms. The
         * 420ms local preroll preserves a normal immediate command's prefix;
         * this legacy guard only applies to the initial post-wake capture.
         * Ready followups consumed calibration before the cue and use no
         * time-based guard. Same-sentence capture also bypasses this guard. */
        if(chain&&!encoder)++command_blocks;
        if(!independent_onset&&ready_prompt&&!encoder&&command_blocks<=8&&ready_echo_only(&echo,mono,v->last_rms)) {
            vad_relisten(v);event=0;
            puts("{\"event\":\"followup_tonal_echo_onset_ignored\",\"raw_preroll_retained\":true,\"audio_uploaded\":false}");
        }
        if(independent_onset&&source_echo_ignored&&onset_source<0)
            puts("{\"event\":\"followup_tonal_echo_onset_ignored\",\"raw_preroll_retained\":true,\"audio_uploaded\":false}");
        if(chain&&!encoder&&command_blocks<=cold_packets){vad_relisten(v);event=0;}
        if(v->last_rms>idle_peak)idle_peak=v->last_rms;
        if(!encoder) {
            if(frontend->mode==MIC_AUTO) {
                for(unsigned ch=0;ch<MIC_CHANNELS;++ch)memcpy(preroll[ch][next],pcm[ch],sizeof(pcm[ch]));
                if(chain)memcpy(preroll[MIC_CHANNELS][next],chain->output,sizeof(pcm[0]));
            } else memcpy(preroll[frontend->selected][next],mono,sizeof(pcm[0]));
            next=(next+1)%preroll_packets;
            if(kept<preroll_packets)++kept;
            if(event!=1) {
                if(now_ms()>=idle_report) {
                    printf("{\"event\":\"wake_idle\",\"noise_rms\":%.1f,\"peak_frame_rms\":%.1f,\"threshold_rms\":%.1f}\n",v->noise,idle_peak,v->last_threshold);
                    mic_frontend_report(frontend,"idle");
                    idle_peak=0;idle_report=now_ms()+10000;
                }
                if(chain)local_chain_consume(chain);
                continue;
            }
            encoder=opus_encoder_create(16000,1,2048,&err);
            if(!encoder||err||opus_encoder_ctl(encoder,4002,24000)||opus_encoder_ctl(encoder,4010,4)){fail("Opus utterance encoder setup failed");goto out;}
            unsigned oldest=(next+preroll_packets-kept)%preroll_packets;
            frontend->locked=1;mic_frontend_report(frontend,"utterance_lock");
            unsigned source=chain&&chain->command_mode?MIC_CHANNELS:frontend->selected;
            if(chain)printf("{\"event\":\"local_command_frontend\",\"source\":\"%s\",\"locked\":true,\"noise_rms\":%.2f,\"cold_guard_ms\":%u}\n",chain->command_mode?"original_beamformer":"six_channel_selection",v->noise,cold_packets*60);
            memset(&q->capture_stats,0,sizeof(q->capture_stats));q->capture_stats.min_rms=32768;q->capture_stats.auxiliary_valid=use_speech&&!speech_failed;
            for(unsigned i=0;i<kept;++i) {
                unsigned slot=(oldest+i)%preroll_packets;const int16_t *packet=preroll[source][slot];
                if(encode_queued(encoder,q,packet))goto out;
                /* Classification counters conservatively exclude pre-roll: its
                 * source may have changed before the encoder locked it. */
                capture_stats_packet(q,packet,0,use_speech&&!speech_failed,0);
            }
            printf("{\"event\":\"speech_detected\",\"preroll_ms\":%u,\"frame_rms\":%.1f,\"threshold_rms\":%.1f,\"onset_block\":%u}\n",kept*60,v->last_rms,v->last_threshold,command_blocks);
            panel_state("capturing");
        } else {
            if(encode_queued(encoder,q,mono))goto out;
            capture_stats_packet(q,mono,(unsigned)speech_votes,use_speech&&!speech_failed,1);
        }
        q->capture_stats.noise_rms=capture_rms_number(v->noise);q->capture_stats.threshold_rms=capture_rms_number(v->last_threshold);
        if(speech_silence*60>q->capture_stats.max_silence_ms)q->capture_stats.max_silence_ms=speech_silence*60;
        if(chain)local_chain_consume(chain);
        if(event==2||q->count>=WAKE_MAX_PACKETS) {
            int closed=capture_finish(&c);
            if(closed){if(chain&&closed==CAPTURE_DISCONTINUITY)rc=LOCAL_CAPTURE_RETRY;goto out;}
            printf("{\"event\":\"utterance_complete\",\"audio_packets\":%u,\"duration_ms\":%u,\"compressed_bytes\":%llu,\"max_duration_reached\":%s,\"short_reads_assembled\":%u}\n",
                q->count,q->count*60,(unsigned long long)q->used,q->count>=WAKE_MAX_PACKETS?"true":"false",c.short_reads);
            q->capture_stats.valid=use_speech;q->capture_stats.max_reason=q->count>=WAKE_MAX_PACKETS;
            rc=0;break;
        }
    }
out:
    {int closed=capture_finish(&c);if(closed&&!stopping)rc=chain&&closed==CAPTURE_DISCONTINUITY?LOCAL_CAPTURE_RETRY:-1;}
    if(encoder)opus_encoder_destroy(encoder);
    if(rc!=0||stopping){wipe(q->bytes,q->used);q->used=0;q->count=0;memset(&q->capture_stats,0,sizeof(q->capture_stats));}
    if(chain)printf("{\"event\":\"local_command_kws_progress\",\"direct_frames\":%llu,\"beam_frames\":%llu,\"ignored_direct_hits\":%u,\"ignored_beam_hits\":%u,\"beam_active\":%s}\n",
                    (unsigned long long)chain->direct_frames,(unsigned long long)chain->beam_frames,
                    chain->ignored_hits[0],chain->ignored_hits[1],chain->afe?"true":"false");
    wipe(preroll,sizeof(preroll));wipe(raw,sizeof(raw));wipe(pcm,sizeof(pcm));
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)wipe(frontend->filters[ch].history,sizeof(frontend->filters[ch].history));
    return stopping?1:rc;
}
static int upload_utterance(struct wire *w,const char *sid,const struct tts_queue *q,int manual) {
    if(!q->count||q->count>WAKE_MAX_PACKETS)return fail("invalid completed utterance");
    if(dialogue_begin(w))return -1;
    struct json_object *start=json_object_new_object();if(!start)return fail("JSON allocation failed");
    add_string(start,"session_id",sid);add_string(start,"type","listen");add_string(start,"state","start");
    add_string(start,"mode","manual");add_string(start,"source",manual?"manual":"s12_wake");
    dialogue_add_id(w,start);
    if(send_json(w,start))return -1;
    /* Standard manual listening: pace buffered 60-ms Opus frames in real time.
     * Capture remains local/complete before upload. This is not streaming ASR.
     * Pacing prevents an async server's decoder queue being flooded by a burst;
     * stop follows the duration of the final frame, without private barriers. */
    int64_t upload_started=now_ms(),reply_deadline=w->deadline;
    w->deadline+=(int64_t)q->count*60;
    for(unsigned i=0;i<q->count;++i) {
        if(send_frame(w,2,q->bytes+q->slices[i].offset,q->slices[i].len))return -1;
        /* Do not catch up in a burst after a stalled write. */
        int64_t next=now_ms()+60;
        while(now_ms()<next) {
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
            /* Fast synthetic capture fixtures may skip wall time. Dedicated
             * real-time pacing tests exercise the unmodified production wait. */
            if(!getenv("S12_TEST_REALTIME_UPLINK"))break;
#endif
            panel_poll();if(stopping||panel_interrupted||!remaining(w->deadline))return -1;
            int64_t left=next-now_ms();struct timespec pause={0,(long)(left>10?10:left)*1000000};
            if(pause.tv_nsec>0)nanosleep(&pause,NULL);
        }
    }
    w->deadline=reply_deadline+(now_ms()-upload_started);
    struct json_object *j=json_object_new_object();if(!j)return fail("JSON allocation failed");
    add_string(j,"session_id",sid);add_string(j,"type","listen");add_string(j,"state","stop");
    dialogue_add_id(w,j);
    if(local_wake_enabled&&w->capture_diagnostics_capable&&q->capture_stats.valid) {
        const struct capture_stats *stats=&q->capture_stats;struct json_object *diagnostic=json_object_new_object();
        if(!diagnostic){json_object_put(j);return fail("capture diagnostic allocation failed");}
        add_string(diagnostic,"reason",stats->max_reason?"max":"silence");
        json_object_object_add(diagnostic,"packets",json_object_new_int((int)q->count));
        json_object_object_add(diagnostic,"duration_ms",json_object_new_int((int)q->count*60));
        json_object_object_add(diagnostic,"speech_frames",json_object_new_int((int)stats->speech_frames));
        json_object_object_add(diagnostic,"vad_frames",json_object_new_int((int)stats->vad_frames));
        json_object_object_add(diagnostic,"max_silence_ms",json_object_new_int((int)stats->max_silence_ms));
        json_object_object_add(diagnostic,"noise_rms",json_object_new_int((int)stats->noise_rms));
        json_object_object_add(diagnostic,"threshold_rms",json_object_new_int((int)stats->threshold_rms));
        json_object_object_add(diagnostic,"min_rms",json_object_new_int((int)stats->min_rms));
        json_object_object_add(diagnostic,"max_rms",json_object_new_int((int)stats->max_rms));
        json_object_object_add(j,"capture_stats",diagnostic);
    }
    if(send_json(w,j))return -1;
    printf("{\"event\":\"utterance_uploaded\",\"audio_packets\":%u,\"queue_barrier_required\":false}\n",q->count);
    return 0;
}
/* 0 terminal turn, 1 ignored wake, 2 server wake-gate error, -1 failure.
 * Only a successfully uploaded audio turn can authorize a matching stop-only
 * terminal. This wire response cannot distinguish no speech from a backend
 * failure, so it conveys no claim about ASR success. An opt-in exact current
 * dialogue/close also permits a stop-only explicit end, including text turns. */
static int receive_turn(struct wire *w,const char *sid,struct audio *a,int wake_mode,int uploaded_audio) {
    struct json_object *j;
    struct message *m=malloc(sizeof(*m));if(!m)return fail("message allocation failed");
    struct tts_queue q;memset(&q,0,sizeof(q));
    if(a->buffer_tts) {
        q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(TTS_BUFFER_PACKETS,sizeof(*q.slices));
        if(!q.bytes||!q.slices){free(q.bytes);free(q.slices);free(m);return fail("TTS RAM buffer allocation failed");}
    }
    int started=0,tts_activity=0,rc=-1;unsigned count=0;
    int64_t music_idle=0,music_hard=0;
    while(++count<20000) {
        panel_poll();if(stopping||panel_interrupted)break;
        if(a->music) {
            if(!remaining(music_hard)){fail("music hard deadline expired");break;}
            if(!remaining(music_idle)){fail("music audio idle deadline expired");break;}
            w->deadline=music_idle<music_hard?music_idle:music_hard;
        }
        if(receive_message(w,m))break;
        if(m->opcode==2) {
            w->response_audio=1;w->command_prompt=COMMAND_PROMPT_NONE;
            if(!started){fail("audio arrived outside TTS turn");break;}
            if(a->buffer_tts&&!a->music) {if(queue_add(&q,m->data,m->len))break;}
            else {panel_state("speaking");if(decode_audio(a,m->data,m->len,w->deadline))break;}
            if(a->music)music_idle=now_ms()+MUSIC_IDLE_TIMEOUT_MS;
            continue;
        }
        j=parse_json(m->data,m->len);
        if(!j){fail("invalid server JSON");break;}
        const char *type=string_field(j,"type"),*state=string_field(j,"state"),*session=string_field(j,"session_id");
        if(type&&!strcmp(type,"dialogue")) {
            dialogue_receive(w,j,sid,m->data,m->len,started,q.count||a->packets);
            json_object_put(j);continue;
        }
        if(sid[0]&&session&&strcmp(session,sid)){json_object_put(j);fail("server session mismatch");break;}
        if(type&&!strcmp(type,"mcp")) {
            int handled=device_mcp_receive(w,j);json_object_put(j);if(handled<0)break;continue;
        }
        if(type&&!strcmp(type,"tts")) {
            if(state&&!strcmp(state,"start")) {
                if(started){json_object_put(j);fail("duplicate TTS start");break;}
                w->command_prompt=COMMAND_PROMPT_NONE;w->quiet_completed=0;
                if(w->dialogue_state==DIALOGUE_LISTEN_ONLY){w->dialogue_invalid=1;w->dialogue_state=DIALOGUE_FALLBACK;}
                started=1;puts("{\"event\":\"tts_start\"}");
            } else if(state&&!strcmp(state,"stop")) {
                if(!started) {
                    if(uploaded_audio&&!tts_activity&&session&&*session&&!strcmp(session,sid)&&w->quiet_followup_capable&&w->followup_request&&
                       !w->response_audio&&!w->dialogue_invalid&&w->dialogue_state==DIALOGUE_LISTEN_ONLY&&w->dialogue_seen==1) {
                        w->quiet_completed=1;puts("{\"event\":\"quiet_followup_accepted\",\"audio_packets\":0,\"new_cue\":false}");json_object_put(j);rc=0;break;
                    }
                    if(session&&*session&&!strcmp(session,sid)&&!w->dialogue_invalid&&w->dialogue_state==DIALOGUE_CLOSE&&w->dialogue_seen) {
                        puts("{\"event\":\"dialogue_closed\",\"audio_packets\":0}");json_object_put(j);rc=0;break;
                    }
                    if(uploaded_audio&&!tts_activity&&session&&*session&&!strcmp(session,sid)) {
                        puts("{\"event\":\"no_audio_terminal\",\"reason\":\"unknown\"}");
                        json_object_put(j);rc=0;break;
                    }
                    json_object_put(j);fail("TTS stop without start");break;
                }
                if(a->buffer_tts&&!a->music) {
                    panel_state("speaking");
                    printf("{\"event\":\"tts_buffered\",\"opus_packets\":%u,\"compressed_bytes\":%llu}\n",q.count,(unsigned long long)q.used);
                    int playback_failed=0;
                    for(unsigned i=0;i<q.count;++i) {
                        panel_poll();if(panel_interrupted){playback_failed=1;break;}
                        if(!remaining(w->deadline)){fail("buffered TTS playback deadline expired");playback_failed=1;break;}
                        if(decode_audio(a,q.bytes+q.slices[i].offset,q.slices[i].len,w->deadline)){playback_failed=1;break;}
                    }
                    if(playback_failed){json_object_put(j);break;}
                }
#ifndef XIAOZHI_TEST_NO_ALSA
                if(a->playback&&drain_playback(a,w->deadline)){json_object_put(j);break;}
#endif
                printf("{\"event\":\"turn_complete\",\"opus_packets\":%u,\"decoded_frames_48000\":%llu,\"playback\":%s,\"buffered\":%s}\n",
                    a->packets,(unsigned long long)a->samples,a->playback?"true":"false",a->buffer_tts&&!a->music?"true":"false");
                json_object_put(j);rc=0;break;
            } else tts_activity=1; /* A sentence/unknown TTS event is not empty. */
        } else if(type&&!strcmp(type,"music")&&state&&!strcmp(state,"playing")) {
            /* Only a same-session marker before the first audio selects the
             * paced song path. Ordinary TTS keeps its bounded whole-turn buffer. */
            if(!started||a->music||q.count||a->packets||a->samples||!session||!*session||strcmp(session,sid)||!remaining(w->deadline)) {
                json_object_put(j);fail("invalid music stream transition");break;
            }
            int64_t now=now_ms();if(now<0){json_object_put(j);fail("music clock unavailable");break;}
            a->music=1;music_idle=now+MUSIC_IDLE_TIMEOUT_MS;music_hard=now+MUSIC_HARD_TIMEOUT_MS;
            free(q.bytes);free(q.slices);memset(&q,0,sizeof(q));
            puts("{\"event\":\"music_stream_start\",\"buffered\":false}");
        } else if(type&&!strcmp(type,"wake")) {
            if(state&&!strcmp(state,"error")) {
                puts("{\"event\":\"wake_gate_error\"}");json_object_put(j);rc=2;break;
            }
            if(!wake_mode){json_object_put(j);continue;}
            if(started||q.count){json_object_put(j);fail("wake event arrived during TTS");break;}
            if(state&&!strcmp(state,"ignored")) {
                puts("{\"event\":\"wake_ignored\"}");json_object_put(j);rc=1;break;
            } else if(state&&!strcmp(state,"accepted"))puts("{\"event\":\"wake_accepted\"}");
            else {json_object_put(j);fail("invalid wake event state");break;}
        } else if(type&&(!strcmp(type,"error")||!strcmp(type,"goodbye"))) {
            json_object_put(j);fail("server reported error or goodbye");break;
        }
        json_object_put(j);
    }
    if(count>=20000)fail("server event limit exceeded");
    free(q.bytes);free(q.slices);free(m);return rc;
}
static int receive_tts(struct wire *w,const char *sid,struct audio *a) {
    return receive_turn(w,sid,a,0,0);
}
static void pause_bounded(unsigned seconds) {
    int64_t end=now_ms()+(int64_t)seconds*1000;
    while(!stopping&&remaining(end)) {
        panel_poll();if(panel_interrupted||ui_feedback_ready())break;
        if(panel_enabled&&!panel.muted&&panel.manual!=panel_manual_used)break;
        int ms=remaining(end);if(ms>100)ms=100;
        struct timespec delay={ms/1000,(ms%1000)*1000000};nanosleep(&delay,NULL);
    }
}
static void local_capture_recover(struct local_chain *chain,struct mic_frontend *frontend,
                                  struct tts_queue *utterance,unsigned *failures) {
    /* Five seconds of actual captured input count as recovery. Sleeping in a
     * long backoff must never reset this counter and defeat its rate limit. */
    if(chain->direct_frames>=500)*failures=0;
    if(*failures<9)++*failures;
    unsigned delay=*failures>=8?30000u:250u<<(*failures-1);
    uint64_t direct=chain->direct_frames,beam=chain->beam_frames;
    wipe(utterance->bytes,utterance->used);utterance->used=0;utterance->count=0;
    local_chain_drop(chain);mic_frontend_init(frontend,MIC_AUTO);
    local_capture_recovering=1;local_wake_state("recovering");
    panel_active=0;panel_interrupted=0;panel_state("error");
    printf("{\"event\":\"local_capture_recovering\",\"consecutive_failures\":%u,\"retry_ms\":%u,\"direct_frames_discarded\":%llu,\"beam_frames_discarded\":%llu,\"old_audio_retried\":false,\"models_reset\":true}\n",
           *failures,delay,(unsigned long long)direct,(unsigned long long)beam);
#ifdef XIAOZHI_TEST_FAKE_CAPTURE
    const char *fast=getenv("S12_TEST_CAPTURE_RETRY_FAST");if(fast&&!strcmp(fast,"1"))delay=(delay+99)/100;
#endif
    int64_t until=now_ms()+delay;
    while(!stopping&&remaining(until)) {
        panel_poll();
        if(panel_interrupted||(panel_enabled&&(panel.muted||panel.manual!=panel_manual_used))||ota_idle_requested())break;
        int ms=remaining(until);if(ms>50)ms=50;
        struct timespec pause={0,ms*1000000};nanosleep(&pause,NULL);
    }
}
/* Called only after local_capture_turn's verified join/drop/close event.
 * Failed, cancelled and already-claimed cues never enter an audio retry loop. */
static void startup_after_probe(const struct audio *options) {
    startup_feedback.pending=0;
    int claimed=startup_claim();
    if(claimed==1) {
        local_wake_state("starting");panel_state("boot");panel_poll();
        if(!stopping&&!panel_interrupted&&!(panel_enabled&&panel.muted)&&!ota_idle_requested()&&!local_prompt("boot",options)) {
            startup_feedback.ready_after_capture=1;
            puts("{\"event\":\"startup_feedback_complete\",\"capture_closed\":true,\"network_required\":false}");
        } else {
            puts("{\"event\":\"startup_feedback_skipped\",\"reason\":\"failed_or_cancelled\",\"retry\":false}");
            panel_state("error");
        }
    } else if(claimed<0)puts("{\"event\":\"startup_feedback_skipped\",\"reason\":\"claim_unavailable\",\"retry\":false}");
}
static void session_wire_close(struct wire *w,int clean) {
    dialogue_reset(w);w->command_feedback_capable=w->quiet_followup_capable=w->capture_diagnostics_capable=0;followup_prompt_pending=followup_quiet_pending=0;
    device_mcp_cancel(w);w->mcp_initialized=0;w->mcp_cache_next=0;wipe(w->mcp_cache,sizeof(w->mcp_cache));
    int partial=w->incomplete||(session_keepalive.wire==w&&(session_keepalive.failed||session_keepalive.bytes));
    session_keepalive_unbind();
    if(w->fd>=0){
        if(clean&&!partial&&remaining(w->deadline)) {
            int64_t short_deadline=now_ms()+1000;if(w->deadline>short_deadline)w->deadline=short_deadline;
            uint8_t code[2]={3,232};(void)send_frame(w,8,code,2);
        }
        close(w->fd);w->fd=-1;
    }
    w->pos=w->used=0;w->writing=w->incomplete=w->receiving=w->mcp_deferred=0;
}
static int conversation_continue(unsigned submitted,int64_t deadline,unsigned completed,unsigned rounds,int answered) {
    return followup_seconds&&answered&&submitted<8&&remaining(deadline)&&(!rounds||completed<rounds)&&
        !stopping&&!panel_interrupted&&!(panel_enabled&&panel.muted)&&!ota_idle_requested();
}
/* This is reached only after the previous turn's ALSA drain and barge worker
 * join/close. The original cue is played locally; only a fresh capture opened
 * afterwards supplies KWS/VAD/ASR input. The listening timer starts there. */
static int followup_open(struct wire *w,const struct audio *options,const struct barge_monitor *monitor,int64_t deadline) {
    panel_poll();
    if(w->fd<0||w->incomplete||!dialogue_wait_ms(w)||!remaining(deadline)||remaining(deadline)<=2000||stopping||panel_interrupted||
       (panel_enabled&&(panel.muted||panel.manual!=panel_manual_used))||ota_idle_requested()||session_keepalive.failed||
       (monitor&&(monitor->opened||monitor->worker_started||monitor->pending)))return 0;
    (void)options;followup_quiet_pending=dialogue_quiet_completed(w);followup_prompt_pending=!followup_quiet_pending;
    local_wake_state("busy");panel_state("thinking");return 1;
}
/* Invoked only after the remote turn completed, ALSA closed, and all barge
 * capture/DSP readers joined and closed. The wire selects a fixed local enum;
 * it cannot name audio, text, a URL or a filesystem path. */
static int command_feedback_play(enum command_prompt prompt,const struct audio *options,const struct barge_monitor *monitor) {
    if(prompt!=COMMAND_PROMPT_FAREWELL&&prompt!=COMMAND_PROMPT_KEY)return 0;
    panel_poll();
    if(stopping||panel_interrupted||ota_idle_requested()||
       (panel_enabled&&(panel.muted||!panel.gain||panel.manual!=panel_manual_used))||
       (monitor&&(monitor->opened||monitor->worker_started||monitor->pending)))return 0;
    /* A successful remote key already confirms its volume change. Merge
     * only volume feedback queued before this boundary; a button or any
     * later physical gesture keeps its own feedback. Never clear after play. */
    if(prompt==COMMAND_PROMPT_KEY) {
        if(ui_feedback.pending==1)ui_feedback.pending=0;
        if(ui_feedback.active==1){ui_feedback.active=0;ui_feedback.offset=0;}
    }
    command_feedback_active=1;panel_active=1;local_wake_state("busy");panel_state("speaking");
    puts("{\"event\":\"command_feedback\",\"capture_closed\":true,\"microphone_uploaded\":false,\"network_required\":false}");
    int result=local_prompt(prompt==COMMAND_PROMPT_FAREWELL?"farewell":"command-key",options);
    command_feedback_active=0;panel_active=0;
    return result?-1:1;
}
static int wake_loop(const struct config *cfg,const struct endpoint *endpoint,
                     unsigned channel,unsigned min_rms,unsigned rounds,int timeout,const struct audio *options) {
    struct in_addr numeric;
    if(inet_pton(AF_INET,endpoint->host,&numeric)!=1||!private_ipv4(numeric.s_addr))
        return fail("wake loop requires a numeric private IPv4 host");
    struct local_chain chain;memset(&chain,0,sizeof(chain));
    if(local_wake_enabled) {
        startup_setup();
        local_wake_state("closed");
        if(local_chain_models(&chain)){local_wake_state("error");return -1;}
    }
    struct tts_queue utterance;memset(&utterance,0,sizeof(utterance));
    utterance.bytes=malloc(TTS_BUFFER_BYTES);utterance.slices=calloc(WAKE_MAX_PACKETS,sizeof(*utterance.slices));
    if(!utterance.bytes||!utterance.slices){free(utterance.bytes);free(utterance.slices);s12_vendor_wake_close(chain.diversity_wake);s12_vendor_wake_close(chain.wake);local_wake_state("error");return fail("utterance RAM allocation failed");}
    struct barge_monitor *monitor=barge_config.enabled?calloc(1,sizeof(*monitor)):NULL;
    if(barge_config.enabled&&!monitor){free(utterance.bytes);free(utterance.slices);local_chain_drop(&chain);return fail("barge monitor allocation failed");}
    struct vad vad;vad_init(&vad,min_rms);unsigned completed=0,failures=0,capture_failures=0;int last_error=0;
    struct mic_frontend frontend;mic_frontend_init(&frontend,local_wake_enabled?MIC_AUTO:channel);
    printf("{\"event\":\"wake_loop_start\",\"local_keyword_spotting\":%s,\"remote_asr_gate\":%s,\"half_duplex\":%s,\"local_barge_in\":%s,\"same_sentence_wake_supported\":%s,\"preroll_ms\":240,\"speech_min_ms\":180,\"end_silence_ms\":1020,\"max_utterance_ms\":12000}\n",
           local_wake_enabled?"true":"false",local_wake_enabled?"false":"true",barge_config.enabled?"false":"true",barge_config.enabled?"true":"false",local_wake_enabled?"true":"false");
    struct wire w;memset(&w,0,sizeof(w));w.fd=-1;
    char sid[128]={0};int rate=0,followup_ready=0;unsigned conversation_turns=0,followup_wait_ms=0;int64_t conversation_deadline=0,retained_onset_deadline=0;
    panel_poll();panel_manual_used=panel.manual;
    while(!stopping&&(!rounds||completed<rounds||(monitor&&monitor->pending))) {
        panel_active=0;panel_interrupted=0;panel_poll();
        if(barge_control_handoff(monitor)){local_capture_recover(&chain,&frontend,&utterance,&capture_failures);continue;}
        if(ota_idle_requested()){session_wire_close(&w,1);followup_ready=0;barge_status.conversation_active=0;}
        if(ota_idle_wait())continue;
        if((!monitor||(!monitor->opened&&!monitor->worker_started&&!monitor->pending))&&
           !(panel_enabled&&panel.manual!=panel_manual_used)) (void)ui_feedback_play(options);
        if(panel_enabled&&panel.muted) {
            session_wire_close(&w,1);followup_ready=0;barge_status.conversation_active=0;
            panel_state("muted");local_wake_state(local_capture_recovering?"recovering":"muted");pause_bounded(1);continue;
        }
        if(local_wake_enabled&&!chain.wake&&local_chain_models(&chain)){last_error=1;local_wake_state("error");break;}
        int manual=!(monitor&&monitor->pending)&&panel_enabled&&panel.manual!=panel_manual_used;
        panel_manual_used=panel.manual;panel_interrupted=0;panel_active=1;
        int following=followup_ready&&!manual&&!(monitor&&monitor->pending)&&remaining(conversation_deadline)&&conversation_turns<8;
        followup_ready=0;
        if(!following){session_wire_close(&w,1);conversation_turns=0;followup_wait_ms=0;conversation_deadline=retained_onset_deadline=0;barge_status.conversation_active=0;}
        utterance.used=0;utterance.count=0;memset(&utterance.capture_stats,0,sizeof(utterance.capture_stats));
        int continuation=0,abort_attempted=0,answered=0,quiet=0;int64_t captured_onset_deadline=0;
        if(following) {
            int opened=followup_open(&w,options,monitor,conversation_deadline);
            if(opened<=0) {
                if(stopping)break;
                session_wire_close(&w,opened==0);barge_status.conversation_active=0;panel_active=0;panel_interrupted=0;panel_state("idle");barge_status_write("idle",1);continue;
            }
            barge_status.conversation_active=1;panel_state("thinking");local_wake_state("busy");
        }
        if(local_wake_enabled&&!following) {
            if(!manual) {
                struct barge_monitor *handoff=monitor&&monitor->pending?monitor:NULL;
                uint32_t before_manual=panel_manual_used;
                int event=local_capture_turn(&chain,handoff?&handoff->frontend:&frontend,min_rms,&utterance,handoff);
                if(panel_enabled&&panel.manual!=before_manual)manual=1;
                if(stopping)break;
                if(event==LOCAL_CAPTURE_RETRY){local_capture_recover(&chain,&frontend,&utterance,&capture_failures);continue;}
                if(event<0){last_error=1;break;}
                if(event==LOCAL_UI_FEEDBACK) {
                    (void)ui_feedback_play(options);panel_state("idle");continue;
                }
                if(event==LOCAL_STARTUP_PROBE) {
                    startup_after_probe(options);
                    continue;
                }
                if(event!=1&&event!=4)continue;
                continuation=event==4;
            }
            local_wake_state("busy");panel_state("listening");
            if(local_prompt(manual?"button":"wake",options)) {
                if(stopping)break;
                panel_active=0;panel_interrupted=0;panel_state("idle");continue;
            }
        }
        followup_capture_deadline=following?conversation_deadline:0;followup_onset_deadline=following?retained_onset_deadline:0;
        unsigned wait_ms=following?followup_wait_ms:local_wake_enabled?8000:0;
        int captured=0,result=-1;
        if(!continuation)do {
            captured=capture_utterance(&vad,&frontend,&utterance,local_wake_enabled?&chain:NULL,wait_ms,options);
            if(captured==LOCAL_UI_FEEDBACK) {
                (void)ui_feedback_play(options);
                if(following&&followup_onset_deadline&&!remaining(followup_onset_deadline)){captured=3;break;}
            }
        }while(captured==LOCAL_UI_FEEDBACK&&!stopping&&!panel_interrupted);
        captured_onset_deadline=following?followup_onset_deadline:0;
        followup_capture_deadline=0;followup_onset_deadline=0;
        if(stopping)break;
        if(captured==LOCAL_CAPTURE_RETRY){session_wire_close(&w,0);barge_status.conversation_active=0;barge_status.last_error_reason="capture_discontinuity";local_capture_recover(&chain,&frontend,&utterance,&capture_failures);continue;}
        if(captured==2||captured==3||captured==FOLLOWUP_SESSION_CLOSED) {
            session_wire_close(&w,captured!=FOLLOWUP_SESSION_CLOSED);barge_status.conversation_active=0;
            if(captured==FOLLOWUP_SESSION_CLOSED){barge_status.last_turn_error="network";barge_status.last_error_reason=barge_error_reason;}
            panel_active=0;panel_state("idle");barge_status_write("idle",1);continue;
        }
        /* A failed follow-up belongs to the old socket. Never reconnect and
         * replay its already captured audio under a new session. */
        if(following&&(session_keepalive.failed||w.fd<0||w.incomplete)) {
            wipe(utterance.bytes,utterance.used);utterance.used=0;utterance.count=0;
            session_wire_close(&w,0);barge_status.conversation_active=0;
            barge_status.last_turn_error="network";barge_status.last_error_reason=barge_error_reason;
            panel_active=0;panel_state("idle");barge_status_write("idle",1);continue;
        }
        if(!captured) {
            /* The completed utterance is already encoded in RAM and capture
             * is closed. Serve a deferred physical control before opening the
             * barge reader, so feedback never becomes microphone input. */
            int feedback_boundary=ui_feedback_play(options);if(stopping)break;
            panel_poll();
            if(feedback_boundary&&(panel_interrupted||(panel_enabled&&(panel.muted||panel.manual!=panel_manual_used))||ota_idle_requested())) {
                /* A control received during the cue wins before any new DSP
                 * reader, connection or upload can use this old utterance. */
                wipe(utterance.bytes,utterance.used);utterance.used=0;utterance.count=0;
                session_wire_close(&w,0);followup_ready=0;barge_status.conversation_active=0;
                panel_active=0;panel_interrupted=0;panel_state("idle");barge_status_write("idle",1);
                puts("{\"event\":\"ui_feedback_turn_discarded\",\"capture_reopened\":false,\"audio_uploaded\":false}");continue;
            }
            panel_state("thinking");
            struct audio a;memset(&a,0,sizeof(a));a.playback=options->playback;a.gain=options->gain;a.buffer_tts=1;
            char token[MAX_TOKEN+1];memset(token,0,sizeof(token));
            if(!conversation_turns)conversation_deadline=now_ms()+120000;
            /* One deadline covers connect, upload, server processing, complete
             * TTS receipt and buffered playback. Idle listening has no timeout. */
            w.deadline=now_ms()+(int64_t)(local_wake_enabled&&timeout>5?5:timeout)*1000;
            barge_failure="none";barge_error_reason="none";
            barge_status.last_turn_error="none";barge_status.last_error_reason="none";
            int monitoring=monitor?barge_start(monitor,&chain,&frontend,min_rms):0;
            if(monitoring) {
                if(monitoring==CAPTURE_DISCONTINUITY)monitor->failed=CAPTURE_DISCONTINUITY;
                else {monitor->failed=-1;panel_interrupted=1;}
            }
            int connected=w.fd>=0&&!session_keepalive.failed;
            if(!monitoring&&!connected) {
                session_wire_close(&w,0);memset(sid,0,sizeof(sid));
                w.deadline=now_ms()+(int64_t)(local_wake_enabled&&timeout>5?5:timeout)*1000;
                if(!load_token(cfg,token)&&!connect_lan(&w,endpoint)&&!handshake(&w,endpoint,cfg,token)&&
                   !send_hello(&w)&&!receive_hello(&w,sid,&rate)&&!session_keepalive_bind(&w,sid)) {
                    connected=1;
                    printf("{\"event\":\"hello\",\"protocol_version\":1,\"server_sample_rate\":%d,\"capture_implemented\":true}\n",rate);
                }
            }
            wipe(token,sizeof(token));
            if(!monitoring&&connected) {
                w.deadline=now_ms()+(int64_t)timeout*1000;++conversation_turns;
                barge_status.conversation_active=followup_seconds!=0;
                if(!upload_utterance(&w,sid,&utterance,manual||local_wake_enabled||following)) {
                    w.followup_request=following;
                    if(following)barge_count(&barge_status.followup_turns);
                    result=receive_turn(&w,sid,&a,!local_wake_enabled,1);
                }
            }
            wipe(token,sizeof(token));
            if(monitor&&(result==0||result==1))barge_confirm_tail(monitor);
            if(w.fd>=0) {
                if(panel_interrupted) {
                    barge_active=0; /* Abort delivery cannot restart local DSP. */
                    close_audio(&a);
                    /* Cancel the remote turn before closing. Local queued TTS
                     * is dropped by close_audio regardless of network state. */
                    panel_active=0;panel_interrupted=0;
                    struct json_object *abort=json_object_new_object();
                    if(abort&&sid[0]&&!w.incomplete){abort_attempted=1;add_string(abort,"type","abort");add_string(abort,"session_id",sid);w.deadline=now_ms()+1000;(void)send_json(&w,abort);}
                    else if(abort)json_object_put(abort);
                    panel_interrupted=1;
                }
            }
            answered=result==0&&a.samples>0;
            quiet=result==0&&following&&dialogue_quiet_completed(&w)&&!a.samples;
            close_audio(&a);++completed;
        }
        if(monitor&&result<0&&!panel_interrupted&&!stopping) {
            barge_status.last_turn_error=!strcmp(barge_failure,"none")?"other":barge_failure;barge_status.last_error_reason=barge_error_reason;
            barge_status_write(barge_status.state?barge_status.state:"error",1);
        }
        barge_worker_join(monitor,1);
        int monitor_error=monitor?monitor->failed:0;
        if(monitor&&!monitor->pending) {
            int closed=barge_stop(monitor);if(closed&&!monitor_error)monitor_error=closed;
        }
        /* Reject old audio on every error; never retry a stale utterance. */
        wipe(utterance.bytes,utterance.used);utterance.used=0;utterance.count=0;
        panel_active=0;
        if(stopping)break;
        if(monitor_error) {
            session_wire_close(&w,0);barge_status.conversation_active=0;
            barge_status.last_turn_error="capture";barge_status.last_error_reason="capture_discontinuity";barge_status_write("error",1);
            panel_interrupted=0;
            if(monitor_error==CAPTURE_DISCONTINUITY){local_capture_recover(&chain,&frontend,&utterance,&capture_failures);continue;}
            last_error=1;local_wake_state("error");break;
        }
        if(monitor&&monitor->pending) {
            session_wire_close(&w,0);barge_status.conversation_active=0;
            barge_count(&barge_status.old_turns_dropped);barge_status_write("interrupted",0);
            printf("{\"event\":\"barge_old_turn_discarded\",\"remote_abort_attempted\":%s,\"tail_audio_reused\":false}\n",abort_attempted?"true":"false");
            panel_interrupted=0;failures=0;last_error=0;continue;
        }
        if(panel_interrupted) {
            session_wire_close(&w,0);barge_status.conversation_active=0;
            puts("{\"event\":\"panel_turn_cancelled\"}");panel_interrupted=0;failures=0;last_error=0;panel_state("idle");barge_status_write("idle",1);
        } else if(result==0||result==1) {
            failures=0;capture_failures=0;last_error=0;
            enum command_prompt command=result==0?dialogue_take_command_prompt(&w):COMMAND_PROMPT_NONE;
            retained_onset_deadline=quiet?captured_onset_deadline:0;
            followup_wait_ms=quiet?(unsigned)remaining(retained_onset_deadline):dialogue_wait_ms(&w);
            followup_ready=followup_wait_ms&&conversation_continue(conversation_turns,conversation_deadline,completed,rounds,answered||quiet);
            barge_status.conversation_active=followup_ready;
            if(!followup_ready){session_wire_close(&w,1);barge_status_write("idle",1);}
            if(command!=COMMAND_PROMPT_NONE&&!answered) {
                followup_ready=0;barge_status.conversation_active=0;
                (void)command_feedback_play(command,options,monitor);
            }
            panel_state("idle");
        } else {
            session_wire_close(&w,0);barge_status.conversation_active=0;panel_state("error");
            last_error=1;if(failures<6)++failures;
            if(local_wake_enabled) {
                if(captured<0){local_wake_state("error");break;}
                panel_active=1;
                if(!strcmp(barge_failure,"network"))(void)local_prompt(local_network_ready()?"cloud":"wifi",options);
                panel_active=0;
                puts("{\"event\":\"local_wake_resuming\",\"old_audio_retried\":false,\"backoff_seconds\":0}");
                panel_state("idle");continue;
            }
            unsigned delay=failures>=6?30u:1u<<(failures-1);
            printf("{\"event\":\"wake_retry\",\"failed_rounds\":%u,\"backoff_seconds\":%u,\"old_audio_retried\":false}\n",failures,delay);
            if(!rounds||completed<rounds)pause_bounded(delay);
        }
    }
    session_wire_close(&w,0);barge_status.conversation_active=0;followup_capture_deadline=0;followup_prompt_pending=followup_quiet_pending=0;
    if(monitor){(void)barge_stop(monitor);barge_current=NULL;wipe(monitor,sizeof(*monitor));free(monitor);}
    wipe(utterance.bytes,TTS_BUFFER_BYTES);free(utterance.bytes);free(utterance.slices);
    local_chain_drop(&chain);if(vad.speech){fvad_free(vad.speech);vad.speech=NULL;}
    local_wake_state(last_error&&!stopping?"error":"closed");
    panel_active=0;panel_state("idle");
    printf("{\"event\":\"wake_loop_stopped\",\"submitted_rounds\":%u,\"interrupted\":%s}\n",completed,stopping?"true":"false");
    return stopping?0:last_error;
}
static int text_turn(struct wire *w,const char *sid,const char *text,struct audio *a) {
    if(dialogue_begin(w))return -1;
    struct json_object *j=json_object_new_object();if(!j)return fail("JSON allocation failed");
    add_string(j,"session_id",sid);add_string(j,"type","listen");add_string(j,"state","detect");add_string(j,"text",text);
    dialogue_add_id(w,j);
    return send_json(w,j)?-1:receive_tts(w,sid,a);
}
static double filter_test_rms(double frequency) {
    struct decimator d;decimator_init(&d);
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];int16_t mono[UPLINK_SAMPLES];
    double energy=0;unsigned total=0;
    for(unsigned block=0;block<4;++block) {
        memset(raw,0,sizeof(raw));
        for(unsigned i=0;i<CAPTURE_CHUNK;++i)
            raw[i*CAPTURE_CHANNELS+2]=(int32_t)(536870912.0*sin(2*PI*frequency*(block*CAPTURE_CHUNK+i)/48000));
        unsigned n=decimate(&d,raw,CAPTURE_CHUNK,2,mono);
        if(n!=UPLINK_SAMPLES)return -1;
        if(!block)continue; /* Exclude the FIR's initial zero-history transient. */
        for(unsigned i=0;i<n;++i)energy+=(double)mono[i]*mono[i];
        total+=n;
    }
    return sqrt(energy/total);
}
static int codec_selftest(void) {
    int error=0,result=-1;
    OpusEncoder *e=opus_encoder_create(16000,1,2048,&error);
    if(!e||error){if(e)opus_encoder_destroy(e);return -1;}
    OpusDecoder *d=opus_decoder_create(48000,1,&error);
    if(!d||error){if(d)opus_decoder_destroy(d);opus_encoder_destroy(e);return -1;}
    int16_t in[UPLINK_SAMPLES],out[DECODE_FRAMES];uint8_t packet[1275];
    for(unsigned i=0;i<UPLINK_SAMPLES;++i)in[i]=(int16_t)(8192*sin(2*PI*1000*i/16000));
    if(opus_encoder_ctl(e,4002,24000)==0) {
        int n=opus_encode(e,in,UPLINK_SAMPLES,packet,sizeof(packet));
        if(n>0&&n<=(int)sizeof(packet)&&opus_decode(d,packet,n,out,DECODE_FRAMES,0)==2880)result=0;
    }
    opus_decoder_destroy(d);opus_encoder_destroy(e);return result;
}
static int vad_selftest(void) {
    struct vad v;vad_init(&v,120);int16_t dc[UPLINK_SAMPLES],tone[UPLINK_SAMPLES];
    for(unsigned i=0;i<UPLINK_SAMPLES;++i){dc[i]=4000;tone[i]=(int16_t)(4000+8192*sin(2*PI*1000*i/16000));}
    for(unsigned i=0;i<100;++i)if(vad_step(&v,dc)||v.last_rms>0.001)return fail("VAD DC rejection failed");
    for(unsigned i=0;i<2;++i)if(vad_step(&v,tone))return fail("VAD transient accepted");
    if(vad_step(&v,dc)||v.active)return fail("VAD streak reset failed");
    for(unsigned i=0;i<WAKE_SPEECH_PACKETS;++i)
        if(vad_step(&v,tone)!=(i+1==WAKE_SPEECH_PACKETS?1:0))return fail("VAD onset duration failed");
    for(unsigned i=0;i<WAKE_SILENCE_PACKETS;++i)
        if(vad_step(&v,dc)!=(i+1==WAKE_SILENCE_PACKETS?2:0))return fail("VAD end silence failed");
    vad_relisten(&v);
    for(unsigned i=0;i<10000;++i)if(vad_step(&v,dc))return fail("VAD silence triggered");
    if(v.noise<2||v.noise>500||WAKE_MAX_PACKETS*60!=12000||WAKE_PREROLL_PACKETS*60!=240)
        return fail("VAD bounds selftest failed");
    /* Quiet-board regression: a former 120-unit minimum / 16-unit noise
     * floor rejected real ~45 RMS speech. DC alone and ~6 RMS noise must stay
     * idle while the low speech burst still needs three consecutive frames. */
    vad_init(&v,30);
    int16_t quiet[UPLINK_SAMPLES],soft[UPLINK_SAMPLES];
    for(unsigned i=0;i<UPLINK_SAMPLES;++i) {
        double noise=8*sin(2*PI*300*i/16000);
        quiet[i]=(int16_t)lrint(4000+noise);
        soft[i]=(int16_t)lrint(4000+noise+64*sin(2*PI*1000*i/16000));
    }
    for(unsigned i=0;i<100;++i)if(vad_step(&v,quiet))return fail("VAD quiet noise triggered");
    if(v.noise<4||v.noise>8||v.last_threshold!=30)return fail("VAD quiet noise adaptation failed");
    for(unsigned i=0;i<WAKE_SPEECH_PACKETS;++i)
        if(vad_step(&v,soft)!=(i+1==WAKE_SPEECH_PACKETS?1:0))return fail("VAD low amplitude speech rejected");
    if(v.last_rms<43||v.last_rms>48)return fail("VAD low amplitude fixture invalid");
    for(unsigned i=0;i<WAKE_SILENCE_PACKETS;++i)
        if(vad_step(&v,quiet)!=(i+1==WAKE_SILENCE_PACKETS?2:0))return fail("VAD low amplitude end silence failed");
    return 0;
}
static int selftest(void) {
    uint8_t hash[20];char b[29];sha1((const uint8_t *)"abc",3,hash);base64(hash,20,b);
    if(strcmp(b,"qZk+NkcGgWq6PiVxeFDCbJzQ2J0="))return fail("SHA1 selftest failed");
    const char *s="dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1((const uint8_t *)s,strlen(s),hash);base64(hash,20,b);
    if(strcmp(b,"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))return fail("RFC6455 selftest failed");
    if(!private_ipv4(htonl(0x0a000001))||!private_ipv4(htonl(0xac100001))||
       private_ipv4(htonl(0xac200001))||private_ipv4(htonl(0x08080808)))return fail("LAN filter selftest failed");
    double pass=filter_test_rms(1000),stop=filter_test_rms(12000);
    if(pass<5700||pass>5900||stop<0||stop>pass*0.01)return fail("capture low-pass/decimator selftest failed");
    if(codec_selftest())return fail("Opus synthetic encoder/decoder selftest failed");
    if(vad_selftest())return -1;
    if(panel_selftest())return fail("panel control parser selftest failed");
    puts("{\"event\":\"selftest_passed\"}");return 0;
}
static void usage(void) {
    fputs("Usage: xiaozhi_client --config FILE (--check-ws | --text TEXT | --listen-once | --wake-loop | --local-wake)\n"
          "       [--mic-channel 0..5|auto | --mic-array] [--seconds 3..10] [--playback] [--buffer-tts] [--gain-permille 0..1000] [--timeout 1..600]\n"
          "       wake-loop: [--vad-rms 30..3000] [--wake-rounds 0..10000] (0 means persistent)\n"
          "       wake-loop: [--panel] (local RAM panel controls and state light)\n"
          "       local-wake: [--barge-in --barge-config FILE] (verified synchronous reference only)\n"
          "       local-wake: [--followup-seconds 0..8] (0 disables; maximum8 turns/120s)\n"
          "       xiaozhi_client --selftest\n"
          "       xiaozhi_client --file-abi-check FILE  (metadata only)\n"
          "--wake-loop: local VAD + remote ASR gate. --local-wake: original S12 offline keyword/BF, local reply, then an 8s command window.\n",stderr);
}
int main(int argc,char **argv) {
    const char *config_path=NULL,*text=NULL,*barge_path=NULL;int check=0,listen=0,wake=0,timeout=90,capture_opts=0,duration_opt=0,wake_opts=0,timeout_opt=0;
    unsigned seconds=4,mic_channel=0,vad_rms=30,wake_rounds=0;
    struct audio audio;memset(&audio,0,sizeof(audio));audio.gain=50;
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc==2&&!strcmp(argv[1],"--selftest"))return selftest()?1:0;
    if(argc==3&&!strcmp(argv[1],"--file-abi-check"))return file_abi_check(argv[2]);
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--config")&&i+1<argc)config_path=argv[++i];
        else if(!strcmp(argv[i],"--text")&&i+1<argc)text=argv[++i];
        else if(!strcmp(argv[i],"--check-ws"))check=1;
        else if(!strcmp(argv[i],"--listen-once"))listen=1;
        else if(!strcmp(argv[i],"--wake-loop"))wake=1;
        else if(!strcmp(argv[i],"--local-wake")){wake=1;local_wake_enabled=1;}
        else if(!strcmp(argv[i],"--playback"))audio.playback=1;
        else if(!strcmp(argv[i],"--buffer-tts"))audio.buffer_tts=1;
        else if(!strcmp(argv[i],"--panel"))panel_enabled=1;
        else if(!strcmp(argv[i],"--barge-in"))barge_config.enabled=1;
        else if(!strcmp(argv[i],"--barge-config")&&i+1<argc)barge_path=argv[++i];
        else if(!strcmp(argv[i],"--followup-seconds")&&i+1<argc) {
            char *end=NULL;errno=0;long value=strtol(argv[++i],&end,10);
            if(errno||!end||!*argv[i]||*end||value<0||value>8){usage();return 2;}followup_seconds=(unsigned)value;
        }
        else if(!strcmp(argv[i],"--mic-array")){mic_channel=MIC_AUTO;capture_opts=1;}
        else if(!strcmp(argv[i],"--mic-channel")&&i+1<argc&&!strcmp(argv[i+1],"auto")) {
            ++i;mic_channel=MIC_AUTO;capture_opts=1;
        }
        else if((!strcmp(argv[i],"--timeout")||!strcmp(argv[i],"--gain-permille")||
                 !strcmp(argv[i],"--seconds")||!strcmp(argv[i],"--mic-channel")||
                 !strcmp(argv[i],"--vad-rms")||!strcmp(argv[i],"--wake-rounds"))&&i+1<argc) {
            int gain=!strcmp(argv[i],"--gain-permille"),duration=!strcmp(argv[i],"--seconds"),channel=!strcmp(argv[i],"--mic-channel");
            int vad=!strcmp(argv[i],"--vad-rms"),rounds=!strcmp(argv[i],"--wake-rounds");
            char *end=NULL;errno=0;
            long v=strtol(argv[++i],&end,10);
            if(errno||!end||!*argv[i]||*end||v<(channel||rounds||gain?0:vad?30:duration?3:1)||v>(channel?5:rounds?10000:vad?3000:duration?10:gain?1000:600)){usage();return 2;}
            if(gain)audio.gain=(int)v;
            else if(duration){seconds=(unsigned)v;duration_opt=1;}
            else if(channel){mic_channel=(unsigned)v;capture_opts=1;}
            else if(vad){vad_rms=(unsigned)v;wake_opts=1;}
            else if(rounds){wake_rounds=(unsigned)v;wake_opts=1;}
            else {timeout=(int)v;timeout_opt=1;}
        } else {usage();return 2;}
    }
    if(!config_path||check+listen+wake+!!text!=1||(panel_enabled&&!wake)||(capture_opts&&!listen&&!wake)||(duration_opt&&!listen)||(wake_opts&&!wake)||(check&&(audio.playback||audio.buffer_tts))||(text&&(!*text||strlen(text)>8192||
       !utf8_valid((const uint8_t *)text,strlen(text))))){usage();return 2;}
    if((barge_config.enabled&&(!local_wake_enabled||!audio.playback||!barge_path))||(!barge_config.enabled&&barge_path)) {
        fail("barge-in requires local-wake, playback and an explicit validated reference config");return 2;
    }
    if(followup_seconds&&!local_wake_enabled){fail("followup requires local-wake");return 2;}
    if(barge_config.enabled&&barge_load_config(barge_path))return 2;
    if(wake&&!timeout_opt)timeout=120;
    signal(SIGPIPE,SIG_IGN);
    if(wake){signal(SIGINT,request_stop);signal(SIGTERM,request_stop);}
    else alarm((unsigned)timeout+(listen?seconds:0)+20);
    struct config cfg;memset(&cfg,0,sizeof(cfg));
    char token[MAX_TOKEN+1];memset(token,0,sizeof(token));
    struct endpoint endpoint;struct wire wire;memset(&wire,0,sizeof(wire));wire.fd=-1;
    int rc=1;char session[128];int rate=0;
    if((local_wake_enabled?load_config_fields(config_path,&cfg):load_config(config_path,&cfg,token))||parse_url(cfg.url,&endpoint))goto done;
    if(wake){wipe(token,sizeof(token));if(local_wake_enabled)(void)s12dbg_start();rc=wake_loop(&cfg,&endpoint,mic_channel,vad_rms,wake_rounds,timeout,&audio)?1:0;s12dbg_stop();goto done;}
    wire.deadline=now_ms()+(timeout>15?15:timeout)*1000;
    if(connect_lan(&wire,&endpoint)||handshake(&wire,&endpoint,&cfg,token))goto done;
    wipe(token,sizeof(token));
    if(send_hello(&wire)||receive_hello(&wire,session,&rate))goto done;
    printf("{\"event\":\"hello\",\"protocol_version\":1,\"server_sample_rate\":%d,\"capture_implemented\":true}\n",rate);
    if(check)rc=0;
    else if(listen) {
        wire.deadline=now_ms()+((int64_t)timeout+seconds)*1000;
        if(capture_upload(&wire,session,mic_channel,seconds))goto done;
        wire.deadline=now_ms()+timeout*1000;rc=receive_tts(&wire,session,&audio)?1:0;
    } else {wire.deadline=now_ms()+timeout*1000;rc=text_turn(&wire,session,text,&audio)?1:0;}
    if(!rc) {uint8_t close_code[2]={3,232};(void)send_frame(&wire,8,close_code,2);}
done:
    wipe(token,sizeof(token));close_audio(&audio);if(wire.fd>=0)close(wire.fd);
    alarm(0);return rc;
}
