/* S12A panel: bounded evdev reader and visible, capped vendor LED attribute.
 * No MCU updater, I2C raw transactions, audio access or NAND writes. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

enum { KEY_UP_I, KEY_DOWN_I, KEY_ENTER_I, KEY_LEFT_I, KEY_RIGHT_I, KEY_COUNT };
enum { ST_BOOT, ST_LISTENING, ST_CAPTURING, ST_THINKING, ST_SPEAKING,
       ST_IDLE, ST_ERROR, ST_MUTED, ST_READY, STATE_COUNT };
enum { EVENT_CHANGED = 1, EVENT_VOLUME = 2 };
enum { LED_BASE_PWM = 32, LED_DAY_PWM = 96, LED_NIGHT_PWM = 16, LED_FALLBACK_PWM = 32 };
#define PANEL_CLOCK_MIN INT64_C(1704067200) /* 2024-01-01 UTC. */
#define PANEL_CLOCK_MAX INT64_C(2147483647) /* Original ARM32 signed time_t. */
#define VOLUME_CACHE 16u
struct volume_reply {
    char nonce[33];
    uint32_t volume, gain, seq;
    const char *reason;
    int used;
};
struct panel_control { uint32_t seq, gain, muted, cancel, manual; };
struct input_absinfo32 { int32_t value, minimum, maximum, fuzz, flat, resolution; };
_Static_assert(sizeof(struct input_absinfo32) == 24, "Linux input_absinfo ioctl ABI");
/* The running target is ARM32: timeval consists of two signed 32-bit words. */
struct arm_input_event { int32_t seconds, useconds; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct arm_input_event) == 16, "ARM32 evdev ABI");
struct panel {
    struct panel_control control;
    unsigned key[KEY_COUNT];
    uint64_t pressed[KEY_COUNT], last_press[KEY_COUNT];
    unsigned char held[KEY_COUNT];
    int directory, input, state, last_led_state, led_failed, input_failed, rgb_enabled, sync_dropped;
    unsigned last_volume_bars, last_led_limit;
    int last_volume_active;
    int volume_pending;
    uint64_t next_scan, next_led, volume_until;
    int ring_available, ring_touching, ring_have_position, ring_direction;
    int32_t ring_minimum, ring_maximum, ring_position, ring_fuzz, ring_accumulated, ring_pending_steps;
    unsigned ring_period, ring_step;
    uint64_t ring_last_step;
    unsigned phase;
    char root[PATH_MAX];
    char led[PATH_MAX];
    unsigned char partial[16];
    size_t partial_size;
    struct volume_reply volume_cache[VOLUME_CACHE];
    unsigned volume_cache_next;
    int volume_starting;
#ifdef S12_PANEL_TEST
    int test_mode;
    uint64_t end_at;
#endif
};
static volatile sig_atomic_t stopping;
static void stop_signal(int signo) { (void)signo; stopping = 1; }
static uint64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
#if defined(__linux__) && defined(__arm__) && defined(__GLIBC__)
extern int __fxstat64(int, int, void *);
_Static_assert(sizeof(struct stat) == 104, "vendor ARM stat64 layout");
_Static_assert(offsetof(struct stat, st_mode) == 16, "vendor ARM stat64 mode");
#endif
static int metadata(int fd, struct stat *s) {
#if defined(__linux__) && defined(__arm__) && defined(__GLIBC__)
    return __fxstat64(3, fd, s);
#else
    return fstat(fd, s);
#endif
}
static int unsigned_field(const char **cursor, uint32_t max, uint32_t *out) {
    const char *s = *cursor;
    uint32_t v = 0;
    if (*s < '0' || *s > '9') return -1;
    do {
        unsigned digit = (unsigned)(*s++ - '0');
        if (digit > max || v > (max - digit) / 10) return -1;
        v = v * 10 + digit;
    } while (*s >= '0' && *s <= '9');
    *out = v; *cursor = s; return 0;
}
static int parse_control(const char *s, struct panel_control *c) {
    struct panel_control n;
    if (strncmp(s, "v1 ", 3)) return -1;
    s += 3;
    if (unsigned_field(&s, UINT32_MAX, &n.seq) || *s++ != ' ' ||
        unsigned_field(&s, 200, &n.gain) || *s++ != ' ' ||
        unsigned_field(&s, 1, &n.muted) || *s++ != ' ' ||
        unsigned_field(&s, UINT32_MAX, &n.cancel) || *s++ != ' ' ||
        unsigned_field(&s, UINT32_MAX, &n.manual) || *s++ != '\n' || *s) return -1;
    *c = n; return 0;
}
static int parse_keymap(const char *s, unsigned key[KEY_COUNT]) {
    unsigned n[KEY_COUNT];
    for (int i = 0; i < KEY_COUNT; i++) {
        uint32_t v;
        if (unsigned_field(&s, 767, &v) || !v || v == 330) return -1;
        n[i] = v;
        for (int j = 0; j < i; j++) if (n[j] == n[i]) return -1;
        if (i + 1 < KEY_COUNT) { if (*s++ != ',') return -1; }
    }
    if (*s) return -1;
    memcpy(key, n, sizeof(n)); return 0;
}
static int read_small(int directory, const char *name, char *buf, size_t cap, int private_file) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? -2 : -1;
    struct stat s;
    if (metadata(fd, &s) || !S_ISREG(s.st_mode) ||
        (private_file && (s.st_uid != geteuid() || (s.st_mode & 077)))) {
        close(fd); return -1;
    }
    ssize_t size;
    do { size = read(fd, buf, cap); } while (size < 0 && errno == EINTR && !stopping);
    close(fd);
    if (size < 1 || (size_t)size >= cap) return -1;
    if (memchr(buf, 0, (size_t)size)) return -1;
    buf[size] = 0; return 0;
}
static int atomic_file(struct panel *p, const char *name, const char *bytes, size_t size) {
    char temp[80];
    int n = snprintf(temp, sizeof(temp), ".%s.%ld.next", name, (long)getpid());
    if (n < 1 || (size_t)n >= sizeof(temp)) return -1;
    int fd = openat(p->directory, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t rc = write(fd, bytes + done, size - done);
        if (rc < 0 && errno == EINTR && !stopping) continue;
        if (rc < 1) break;
        done += (size_t)rc;
    }
    int result = done == size ? 0 : -1;
    if (close(fd)) result = -1;
    if (!result && renameat(p->directory, temp, p->directory, name)) result = -1;
    if (result) unlinkat(p->directory, temp, 0);
    return result;
}
static int publish(struct panel *p) {
    char text[100];
    int n = snprintf(text, sizeof(text), "v1 %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
                     p->control.seq, p->control.gain, p->control.muted, p->control.cancel, p->control.manual);
    return n < 0 || (size_t)n >= sizeof(text) ? -1 : atomic_file(p, "panel.control", text, (size_t)n);
}
static void increment(uint32_t *v) { if (*v != UINT32_MAX) ++*v; }
static int act(struct panel *p, int key) {
    switch (key) {
    case KEY_UP_I: if (p->control.gain < 200) p->control.gain = p->control.gain > 190 ? 200 : p->control.gain + 10; else return 0; break;
    case KEY_DOWN_I: if (p->control.gain) p->control.gain = p->control.gain < 10 ? 0 : p->control.gain - 10; else return 0; break;
    case KEY_ENTER_I: increment(&p->control.manual); increment(&p->control.cancel); break;
    case KEY_LEFT_I: p->control.muted ^= 1; increment(&p->control.cancel); break;
    case KEY_RIGHT_I: increment(&p->control.cancel); break;
    default: return 0;
    }
    increment(&p->control.seq);
    return 1;
}
static void ring_reset(struct panel *p) {
    p->ring_touching = p->ring_have_position = 0;
    p->ring_accumulated = p->ring_pending_steps = 0; p->ring_last_step = 0;
}
static int ring_configure(struct panel *p, const struct input_absinfo32 *info) {
    ring_reset(p); p->ring_available = 0;
    int64_t period = (int64_t)info->maximum - info->minimum + 1;
    if (info->minimum < 0 || info->maximum > 65535 || period < 16 || period > 65536 ||
        info->fuzz < 0 || info->fuzz > period / 8 || info->flat < 0) return -1;
    p->ring_minimum = info->minimum; p->ring_maximum = info->maximum;
    p->ring_period = (unsigned)period; p->ring_fuzz = info->fuzz;
    p->ring_step = ((unsigned)period + 19) / 20;
    if (p->ring_step <= (unsigned)info->fuzz * 2) p->ring_step = (unsigned)info->fuzz * 2 + 1;
    p->ring_available = 1; return 0;
}
static int ring_flush(struct panel *p, uint64_t ms) {
    if (!p->ring_available || !p->ring_pending_steps) return 0;
    if (p->ring_last_step && ms - p->ring_last_step < 80) return 0;
    int32_t steps = p->ring_pending_steps;
    if (steps > 4) steps = 4;
    if (steps < -4) steps = -4;
    p->ring_pending_steps -= steps; p->ring_last_step = ms;
    int changed = 0, key = steps > 0 ? KEY_UP_I : KEY_DOWN_I;
    for (unsigned n = (unsigned)(steps > 0 ? steps : -steps); n; --n) changed |= act(p, key);
    return EVENT_VOLUME | (changed ? EVENT_CHANGED : 0);
}
static int ring_event(struct panel *p, const struct arm_input_event *ev, uint64_t ms) {
    if (!p->ring_available) return 0;
    if (ev->type == 1 && ev->code == 330) {
        if (ev->value == 0) {
            p->ring_touching = p->ring_have_position = 0; p->ring_accumulated = 0;
        }
        else if (ev->value == 1 && !p->ring_touching) {
            p->ring_have_position = 0; p->ring_accumulated = 0; p->ring_touching = 1;
        }
        return 0;
    }
    if (ev->type != 3 || ev->code != 0 || !p->ring_touching) return 0;
    if (ev->value < p->ring_minimum || ev->value > p->ring_maximum) {
        p->ring_have_position = 0; p->ring_accumulated = 0; return 0;
    }
    if (!p->ring_have_position) {
        p->ring_position = ev->value; p->ring_have_position = 1; return 0;
    }
    int32_t delta = ev->value - p->ring_position;
    if (delta > (int32_t)p->ring_period / 2) delta -= (int32_t)p->ring_period;
    else if (delta < -(int32_t)p->ring_period / 2) delta += (int32_t)p->ring_period;
    if (delta <= p->ring_fuzz && delta >= -p->ring_fuzz) return 0;
    p->ring_position = ev->value;
    /* Large teleports are ambiguous, never a reason to jump the gain. */
    if (delta > (int32_t)p->ring_period / 4 || delta < -(int32_t)p->ring_period / 4) {
        p->ring_accumulated = 0; return 0;
    }
    p->ring_accumulated += delta * p->ring_direction;
    int32_t steps = p->ring_accumulated / (int32_t)p->ring_step;
    p->ring_accumulated -= steps * (int32_t)p->ring_step;
    p->ring_pending_steps += steps;
    if (p->ring_pending_steps > 20) p->ring_pending_steps = 20;
    if (p->ring_pending_steps < -20) p->ring_pending_steps = -20;
    return ring_flush(p, ms);
}
static int event(struct panel *p, const struct arm_input_event *ev, uint64_t ms) {
    if (ev->type == 0 && ev->code == 3) {
        memset(p->held, 0, sizeof(p->held)); ring_reset(p);
        p->volume_pending = 0; p->volume_until = 0; p->sync_dropped = 1; return 0;
    }
    if (p->sync_dropped) {
        if (ev->type == 0 && ev->code == 0) p->sync_dropped = 0;
        return 0;
    }
    if ((ev->type == 1 && ev->code == 330) || (ev->type == 3 && ev->code == 0)) return ring_event(p, ev, ms);
    if (ev->type != 1 || (ev->value != 0 && ev->value != 1)) return 0;
    int key;
    for (key = 0; key < KEY_COUNT; key++) if (p->key[key] == ev->code) break;
    if (key == KEY_COUNT) return 0;
    if (ev->value == 1) {
        if (p->held[key] || (p->last_press[key] && ms - p->last_press[key] < 80)) return 0;
        p->held[key] = 1; p->pressed[key] = ms; p->last_press[key] = ms;
        /* A press at the gain clamp still gives visual feedback, without
         * inventing a control change or incrementing any control counter. */
        return key == KEY_UP_I || key == KEY_DOWN_I ? EVENT_VOLUME | (act(p, key) ? EVENT_CHANGED : 0) : 0;
    }
    if (!p->held[key]) return 0;
    p->held[key] = 0;
    if (key == KEY_UP_I || key == KEY_DOWN_I || ms - p->pressed[key] > 1500) return 0;
    return act(p, key);
}
static int apply_action(struct panel *p, int action, struct panel_control previous) {
    if ((action & EVENT_CHANGED) && publish(p)) {
        p->control = previous;
        if (!p->input_failed) fprintf(stderr, "panel: control publish failed; input disabled\n");
        p->input_failed = 1;
        if (p->input >= 0) close(p->input);
        p->input = -1; ring_reset(p); return -1;
    }
    if (action & EVENT_VOLUME) p->volume_pending = 1;
    else if (action & EVENT_CHANGED) {
        p->volume_pending = 0; p->volume_until = 0;
        p->ring_pending_steps = 0; p->ring_accumulated = 0;
    }
    return 0;
}
/* One C-main producer atomically publishes this fixed private request. Claim
 * by rename before replying: an ACK consumer can immediately publish its next
 * request without our later cleanup accidentally removing the new command. */
struct volume_request { char nonce[33]; uint32_t volume; uint64_t deadline; };
static int volume_request_read(struct panel *p, struct volume_request *request) {
    int fd = openat(p->directory, ".panel-volume.claim", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? -2 : -1;
    struct stat st; unsigned owner = 0;
#ifdef S12_PANEL_TEST
    owner = (unsigned)geteuid();
#endif
    int good = !metadata(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == owner &&
               st.st_nlink == 1 && (st.st_mode & 07777) == 0600 && st.st_size > 0 && st.st_size < 128;
    char raw[128]; size_t at = 0;
    if (good) while (at < (size_t)st.st_size) {
        ssize_t n = read(fd, raw + at, (size_t)st.st_size - at);
        if (n < 0 && errno == EINTR && !stopping) continue;
        if (n <= 0) { good = 0; break; } at += (size_t)n;
    }
    char extra; ssize_t more = good ? read(fd, &extra, 1) : -1; close(fd);
    if (!good || more || memchr(raw, 0, at)) return -1;
    raw[at] = 0;
    if (strncmp(raw, "v1 ", 3)) return -1;
    const char *s = raw + 3;
    for (unsigned i = 0; i < 32; ++i) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return -1;
        request->nonce[i] = *s++;
    }
    request->nonce[32] = 0;
    if (*s++ != ' ' || unsigned_field(&s, 100, &request->volume) || *s++ != ' ') return -1;
    uint64_t deadline = 0;
    if (*s < '0' || *s > '9') return -1;
    do {
        unsigned digit = (unsigned)(*s++ - '0');
        if (deadline > (UINT64_MAX - digit) / 10) return -1;
        deadline = deadline * 10 + digit;
    } while (*s >= '0' && *s <= '9');
    if (*s++ != '\n' || *s) return -1;
    request->deadline = deadline; return 0;
}
static int volume_ack(struct panel *p, const struct volume_reply *reply) {
    char raw[160]; int n;
    if (reply->reason) n = snprintf(raw, sizeof(raw), "v1 %s failed %s\n", reply->nonce, reply->reason);
    else n = snprintf(raw, sizeof(raw), "v1 %s ok %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
                      reply->nonce, reply->volume, reply->gain, reply->seq);
    return n < 0 || (size_t)n >= sizeof(raw) ? -1 : atomic_file(p, "panel-volume.ack", raw, (size_t)n);
}
static void volume_tick(struct panel *p) {
    struct volume_request request;
    int status = volume_request_read(p, &request);
    if (status == -2) {
        if (renameat(p->directory, "panel-volume.request", p->directory, ".panel-volume.claim")) {
            if (errno == ENOENT) p->volume_starting = 0;
            return;
        }
        status = volume_request_read(p, &request);
    }
    if (status) { (void)unlinkat(p->directory, ".panel-volume.claim", 0); return; }
    struct volume_reply *reply = NULL, conflict;
    for (unsigned i = 0; i < VOLUME_CACHE; ++i)
        if (p->volume_cache[i].used && !strcmp(request.nonce, p->volume_cache[i].nonce)) { reply = &p->volume_cache[i]; break; }
    if (reply && reply->volume != request.volume) {
        memset(&conflict, 0, sizeof(conflict)); strcpy(conflict.nonce, request.nonce);
        conflict.reason = "nonce_conflict"; reply = &conflict;
    } else if (!reply) {
        reply = &p->volume_cache[p->volume_cache_next]; p->volume_cache_next = (p->volume_cache_next + 1) % VOLUME_CACHE;
        memset(reply, 0, sizeof(*reply)); strcpy(reply->nonce, request.nonce); reply->used = 1; reply->volume = request.volume;
        uint64_t ms = now_ms();
        if (p->volume_starting) reply->reason = "daemon_restart";
        else if (!ms || request.deadline <= ms) reply->reason = "expired";
        else if (request.deadline - ms > 2000) reply->reason = "invalid_deadline";
        else {
            struct panel_control previous = p->control;
            p->control.gain = request.volume * 2;
            if (p->control.gain != previous.gain) increment(&p->control.seq);
            /* Recheck just before commit; an expired caller's request must
             * never start a late state change after a blocked daemon resumes. */
            if (request.deadline <= now_ms()) { p->control = previous; reply->reason = "expired"; }
            else if (publish(p)) { p->control = previous; reply->reason = "publish_failed"; }
            else {
                reply->gain = p->control.gain; reply->seq = p->control.seq; p->volume_pending = 1;
                p->ring_pending_steps = p->ring_accumulated = 0; p->ring_have_position = 0;
            }
        }
    }
    /* Publish errors leave the claimed request and cached outcome in place.
     * Only the ACK is retried; a later touch cannot be undone by that retry. */
    if (!volume_ack(p, reply)) (void)unlinkat(p->directory, ".panel-volume.claim", 0);
}
static void input_bytes(struct panel *p, const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        p->partial[p->partial_size++] = bytes[i];
        if (p->partial_size == sizeof(struct arm_input_event)) {
            struct arm_input_event ev;
            memcpy(&ev, p->partial, sizeof(ev));
            p->partial_size = 0;
            struct panel_control previous = p->control;
            uint64_t ms = now_ms();
            int action = event(p, &ev, ms);
            if (apply_action(p, action, previous)) return;
        }
    }
}
static int path_under(struct panel *p, char *out, size_t cap, const char *suffix) {
    int n = snprintf(out, cap, "%s%s", p->root, suffix);
    return n < 1 || (size_t)n >= cap ? -1 : 0;
}
static int read_path(const char *path, char *buf, size_t cap) { return read_small(AT_FDCWD, path, buf, cap, 0); }
#ifdef S12_PANEL_TEST
static int test_absinfo(const char *path, struct input_absinfo32 *info) {
    char raw[160]; if (read_path(path, raw, sizeof(raw))) return -1;
    const char *cursor = raw; int32_t values[6];
    for (unsigned i = 0; i < 6; i++) {
        uint32_t value;
        if (unsigned_field(&cursor, INT32_MAX, &value)) return -1;
        values[i] = (int32_t)value;
        if (*cursor++ != (i == 5 ? '\n' : ' ')) return -1;
    }
    if (*cursor) return -1;
    memcpy(info, values, sizeof(*info)); return 0;
}
#endif
static int scan_input(struct panel *p) {
    char directory[PATH_MAX];
    if (path_under(p, directory, sizeof(directory), "/sys/class/input")) return -1;
    DIR *d = opendir(directory);
    if (!d) return -1;
    struct dirent *entry;
    int result = -1, count = 0;
    while ((entry = readdir(d)) != NULL && count++ < 128) {
        if (strncmp(entry->d_name, "event", 5)) continue;
        const char *number = entry->d_name + 5;
        uint32_t event_number;
        if (unsigned_field(&number, 255, &event_number) || *number) continue;
        char path[PATH_MAX], name[80], dev[32];
        int n = snprintf(path, sizeof(path), "%s/%s/device/name", directory, entry->d_name);
        if (n < 1 || (size_t)n >= sizeof(path) || read_path(path, name, sizeof(name)) || strcmp(name, "keys-cyttsp\n")) continue;
        n = snprintf(path, sizeof(path), "%s/%s/dev", directory, entry->d_name);
        if (n < 1 || (size_t)n >= sizeof(path) || read_path(path, dev, sizeof(dev))) continue;
        const char *s = dev;
        uint32_t device_major, device_minor;
        if (unsigned_field(&s, 13, &device_major) || device_major != 13 || *s++ != ':' ||
            unsigned_field(&s, 255, &device_minor) || *s++ != '\n' || *s) continue;
        n = snprintf(path, sizeof(path), "%s/dev/input/event%u", p->root, event_number);
        if (n < 1 || (size_t)n >= sizeof(path)) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) continue;
        struct stat statbuf;
        int valid = !metadata(fd, &statbuf);
#ifdef S12_PANEL_TEST
        if (p->test_mode) valid = valid && S_ISFIFO(statbuf.st_mode);
        else
#endif
        {
#if defined(__linux__)
            char realname[80]; memset(realname, 0, sizeof(realname));
            valid = valid && S_ISCHR(statbuf.st_mode) && major(statbuf.st_rdev) == device_major &&
                    minor(statbuf.st_rdev) == device_minor &&
                    ioctl(fd, 0x80504506UL, realname) > 0 && memchr(realname, 0, sizeof(realname)) &&
                    !strcmp(realname, "keys-cyttsp");
#else
            valid = 0;
#endif
        }
        if (!valid) { close(fd); continue; }
        struct input_absinfo32 abs;
        int abs_status;
#ifdef S12_PANEL_TEST
        if (p->test_mode) {
            n = snprintf(path, sizeof(path), "%s/%s/device/abs_x", directory, entry->d_name);
            abs_status = n > 0 && (size_t)n < sizeof(path) ? test_absinfo(path, &abs) : -1;
        } else
#endif
        abs_status = ioctl(fd, 0x80184540UL, &abs);
        if (abs_status < 0 || ring_configure(p, &abs)) {
            p->ring_available = 0; ring_reset(p);
            fprintf(stderr, "panel: ring ABS_X unavailable; discrete touch remains enabled\n");
        } else fprintf(stderr, "panel: ring ABS_X=%d..%d direction=%d step=%u\n",
                       p->ring_minimum, p->ring_maximum, p->ring_direction, p->ring_step);
        result = fd; break;
    }
    closedir(d); return result;
}
static int read_state(struct panel *p) {
    static const char *const labels[STATE_COUNT] = {
        "boot\n", "listening\n", "capturing\n", "thinking\n", "speaking\n", "idle\n", "error\n", "muted\n", "ready\n"
    };
    if (p->control.muted) return ST_MUTED;
    char buf[24];
    if (read_small(p->directory, "ui.state", buf, sizeof(buf), 0)) return ST_IDLE;
    for (int i = 0; i < STATE_COUNT; i++) if (!strcmp(buf, labels[i])) return i;
    return ST_IDLE;
}
static int led_one(struct panel *p, unsigned index, uint32_t rgb) {
    if (index > 11 || (rgb & 255) > LED_DAY_PWM || ((rgb >> 8) & 255) > LED_DAY_PWM ||
        (rgb >> 16) > LED_DAY_PWM) return -1;
    int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
#ifdef S12_PANEL_TEST
    if (p->test_mode) flags |= O_APPEND;
#endif
    int fd = open(p->led, flags);
    if (fd < 0) return -1;
    char command[32];
    int n = snprintf(command, sizeof(command), "%u 0x%06" PRIx32 "\n", index, rgb);
    ssize_t rc;
    do { rc = write(fd, command, (size_t)n); } while (rc < 0 && errno == EINTR && !stopping);
    int result = rc == n ? 0 : -1;
    if (close(fd)) result = -1;
    return result;
}
static uint32_t color(int state, unsigned phase, int rgb_enabled) {
    static const unsigned pulse[] = { 8, 12, 20, 32, 20, 12 };
    unsigned value = pulse[phase % 6];
    /* Semantic colors here use conventional RRGGBB. Conversion to the vendor
     * BBGGRR attribute belongs only at the final write boundary below. */
    if (!rgb_enabled) {
        switch (state) {
        case ST_BOOT: case ST_LISTENING: case ST_THINKING: case ST_SPEAKING: return value;
        case ST_CAPTURING: case ST_MUTED: case ST_READY: return 32;
        case ST_ERROR: return phase % 2 ? 0 : 16;
        default: return 0;
        }
    }
    switch (state) {
    case ST_BOOT: return value;
    case ST_READY: return 0x002020;
    case ST_LISTENING: return value | (value << 8);
    case ST_CAPTURING: return 0x000020;
    case ST_THINKING: return value << 8 | value << 16;
    case ST_SPEAKING: return value << 8;
    case ST_ERROR: return phase % 2 ? 0 : 0x100000;
    case ST_MUTED: return 0x200800;
    default: return 0;
    }
}
static uint32_t vendor_rgb(uint32_t logical_rgb) {
    /* Exact is31fl3236a.ko Usage: bits 0..7 RED, 8..15 GREEN, 16..23 BLUE. */
    return ((logical_rgb & 255) << 16) | (logical_rgb & 0xff00) | ((logical_rgb >> 16) & 255);
}
static unsigned brightness_limit(int64_t seconds) {
    if (seconds < PANEL_CLOCK_MIN || seconds > PANEL_CLOCK_MAX) return LED_FALLBACK_PWM;
    /* Fixed Beijing UTC+8, independent of libc TZ, locale or daylight saving.
     * Preserve 64-bit arithmetic when crossing the ARM32 2038 boundary. */
    int64_t beijing_seconds = seconds + INT64_C(28800);
    unsigned local_seconds = (unsigned)(beijing_seconds % INT64_C(86400));
    return local_seconds >= 7u * 3600u && local_seconds < 22u * 3600u ? LED_DAY_PWM : LED_NIGHT_PWM;
}
static int64_t panel_wall_time(struct panel *p) {
#ifdef S12_PANEL_TEST
    if (p->test_mode) {
        char text[40], *end;
        if (read_small(p->directory, "test.clock", text, sizeof(text), 1)) return -1;
        errno = 0;
        long long seconds = strtoll(text, &end, 10);
        if (errno || end == text || *end++ != '\n' || *end) return -1;
        return (int64_t)seconds;
    }
#else
    (void)p;
#endif
    time_t seconds = time(NULL);
    return seconds == (time_t)-1 ? -1 : (int64_t)seconds;
}
static uint32_t brightness_rgb(uint32_t rgb, unsigned limit) {
    if (limit > 255) limit = 255;
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned component = (rgb >> shift) & 255;
        if (component > LED_BASE_PWM) component = LED_BASE_PWM;
        component = (component * limit + LED_BASE_PWM / 2) / LED_BASE_PWM;
        result |= (uint32_t)component << shift;
    }
    return result;
}
static int animated(int state) { return state == ST_BOOT || state == ST_LISTENING || state == ST_THINKING || state == ST_SPEAKING || state == ST_ERROR; }
static unsigned volume_bars(uint32_t gain) { return (gain * 12 + 199) / 200; }
static void led_tick(struct panel *p, uint64_t ms, int force_off) {
    if (!force_off && ms < p->next_led) return;
    int state = force_off ? ST_IDLE : read_state(p);
    unsigned limit = force_off ? 0 : brightness_limit(panel_wall_time(p));
    p->next_led = ms + 1000; /* At most 12 attribute writes per second. */
    /* Privacy indication overrides both a pending and a rendered volume bar,
     * including when the voice client itself publishes ui.state=muted. */
    if (force_off || state == ST_MUTED) {
        p->volume_pending = 0; p->volume_until = 0;
    } else if (p->volume_pending) {
        p->volume_pending = 0; p->volume_until = ms + 1000;
    }
    int volume_active = ms < p->volume_until;
    unsigned bars = volume_active ? volume_bars(p->control.gain) : 0;
    if (!force_off && state == p->last_led_state && bars == p->last_volume_bars && limit == p->last_led_limit &&
        volume_active == p->last_volume_active && (volume_active || !animated(state)) && !p->led_failed) return;
    if (state != p->last_led_state) p->phase = 0;
    p->last_led_state = state;
    p->last_volume_bars = bars;
    p->last_volume_active = volume_active;
    p->last_led_limit = limit;
    uint32_t rgb = volume_active ? (p->rgb_enabled ? 0x202020 : 32) : color(state, p->phase++, p->rgb_enabled);
    rgb = brightness_rgb(rgb, limit);
    if (p->rgb_enabled) rgb = vendor_rgb(rgb);
    for (unsigned index = 0; index < 12; index++) {
        if (led_one(p, index, volume_active && index >= bars ? 0 : rgb)) {
            if (!p->led_failed) fprintf(stderr, "panel: LED unavailable; voice unaffected\n");
            p->led_failed = 1; return;
        }
    }
    p->led_failed = 0;
}
static int selftest(void) {
    struct panel p; memset(&p, 0, sizeof(p));
    if (parse_keymap("103,108,28,105,106", p.key)) return 1;
    p.control.gain = 50;
    struct panel_control c;
    if (parse_control("v1 0 50 0 0 0\n", &c) || c.gain != 50 ||
        !parse_control("v1 0 201 0 0 0\n", &c) || !parse_control("v1 0 50 2 0 0\n", &c) ||
        !parse_control("v1 4294967296 50 0 0 0\n", &c) || !parse_keymap("103,103,28,105,106", p.key)) return 1;
    if (!act(&p, KEY_UP_I) || p.control.gain != 60 || !act(&p, KEY_LEFT_I) || !p.control.muted ||
        !act(&p, KEY_ENTER_I) || p.control.cancel != 2 || p.control.manual != 1) return 1;
    if (parse_control("v1 0 0 0 0 0\n", &c) || c.gain || volume_bars(0) ||
        vendor_rgb(0x200800) != 0x000820 || vendor_rgb(0x000020) != 0x200000 ||
        vendor_rgb(0x000808) != 0x080800 || volume_bars(10) != 1 ||
        volume_bars(50) != 3 || volume_bars(60) != 4 || volume_bars(200) != 12) return 1;
    if (!animated(ST_BOOT) || animated(ST_READY) || color(ST_BOOT, 0, 1) != 0x000008 ||
        color(ST_BOOT, 3, 1) != 0x000020 || color(ST_READY, 0, 1) != 0x002020 ||
        color(ST_READY, 5, 1) != 0x002020 || color(ST_READY, 0, 0) != 32) return 1;
    for (int s = 0; s < STATE_COUNT; s++) for (unsigned phase = 0; phase < 6; phase++) {
        uint32_t v = color(s, phase, 1);
        if ((v & 255) > 32 || ((v >> 8) & 255) > 32 || ((v >> 16) & 255) > 32) return 1;
    }
    if (brightness_limit(PANEL_CLOCK_MIN - 1) != LED_FALLBACK_PWM ||
        brightness_limit(-1) != LED_FALLBACK_PWM || brightness_limit(0) != LED_FALLBACK_PWM ||
        brightness_limit(PANEL_CLOCK_MAX + 1) != LED_FALLBACK_PWM ||
        brightness_limit(PANEL_CLOCK_MIN + 13 * 3600 + 59 * 60 + 59) != LED_DAY_PWM ||
        brightness_limit(PANEL_CLOCK_MIN + 14 * 3600) != LED_NIGHT_PWM ||
        brightness_limit(PANEL_CLOCK_MIN + 16 * 3600) != LED_NIGHT_PWM ||
        brightness_limit(PANEL_CLOCK_MIN + 22 * 3600 + 59 * 60 + 59) != LED_NIGHT_PWM ||
        brightness_limit(PANEL_CLOCK_MIN + 23 * 3600) != LED_DAY_PWM ||
        brightness_rgb(0x200800, LED_DAY_PWM) != 0x601800 ||
        brightness_rgb(0x200800, LED_NIGHT_PWM) != 0x100400 ||
        brightness_rgb(0x202020, 255) != 0xffffff || brightness_rgb(0, 255)) return 1;
    static const unsigned day_pulse[] = {24, 36, 60, 96, 60, 36};
    static const unsigned night_pulse[] = {4, 6, 10, 16, 10, 6};
    for (unsigned phase = 0; phase < 6; phase++) {
        if (brightness_rgb(color(ST_LISTENING, phase, 0), LED_DAY_PWM) != day_pulse[phase] ||
            brightness_rgb(color(ST_LISTENING, phase, 0), LED_NIGHT_PWM) != night_pulse[phase]) return 1;
        for (int s = 0; s < STATE_COUNT; s++) {
            uint32_t value = brightness_rgb(color(s, phase, 1), LED_DAY_PWM);
            if ((value & 255) > LED_DAY_PWM || ((value >> 8) & 255) > LED_DAY_PWM ||
                (value >> 16) > LED_DAY_PWM) return 1;
        }
    }
    struct input_absinfo32 abs = {0, 0, 240, 0, 0, 0};
    p.ring_direction = 1; p.control.gain = 50;
    if (ring_configure(&p, &abs)) return 1;
    struct arm_input_event touch = {0, 0, 1, 330, 1}, position = {0, 0, 3, 0, 230};
    if (event(&p, &touch, 100) || event(&p, &position, 100) || p.control.gain != 50) return 1;
    position.value = 5;
    if (event(&p, &position, 200) != (EVENT_CHANGED | EVENT_VOLUME) || p.control.gain != 60) return 1;
    touch.value = 0;
    if (event(&p, &touch, 300) || p.ring_have_position || p.ring_accumulated) return 1;
    position.value = 230;
    if (event(&p, &position, 400) || p.control.gain != 60) return 1;
    puts("S12_PANEL_SELFTEST_OK"); return 0;
}
int main(int argc, char **argv) {
    struct panel p; memset(&p, 0, sizeof(p));
    p.input = -1; p.directory = -1; p.last_led_state = -1; p.control.gain = 50; p.ring_direction = 1;
    const char *keymap = "103,108,28,105,106";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest") && argc == 2) return selftest();
        if (!strcmp(argv[i], "--keymap") && i + 1 < argc) { keymap = argv[++i]; continue; }
        if (!strcmp(argv[i], "--ring-direction") && i + 1 < argc) {
            const char *direction = argv[++i];
            if (!strcmp(direction, "1")) p.ring_direction = 1;
            else if (!strcmp(direction, "-1")) p.ring_direction = -1;
            else return 2;
            continue;
        }
        if (!strcmp(argv[i], "--rgb-confirmed")) { p.rgb_enabled = 1; continue; }
#ifdef S12_PANEL_TEST
        if (!strcmp(argv[i], "--test-root") && i + 1 < argc) {
            const char *root = argv[++i];
            if (*root != '/' || strlen(root) + 100 >= sizeof(p.root)) return 2;
            strcpy(p.root, root); p.test_mode = 1; continue;
        }
        if (!strcmp(argv[i], "--run-for-ms") && i + 1 < argc) {
            const char *s = argv[++i]; uint32_t ms;
            if (unsigned_field(&s, 60000, &ms) || *s || !ms) return 2;
            p.end_at = now_ms() + ms; continue;
        }
#endif
        fprintf(stderr, "panel: invalid option\n"); return 2;
    }
    if (parse_keymap(keymap, p.key)) return 2;
    char runtime[PATH_MAX];
    if (path_under(&p, runtime, sizeof(runtime), "/run/s12") ||
        path_under(&p, p.led, sizeof(p.led), "/sys/devices/i2c-1/1-003c/led_rgb")) return 2;
    p.directory = open(runtime, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    struct stat statbuf;
    if (p.directory < 0 || metadata(p.directory, &statbuf) || statbuf.st_uid != geteuid() || (statbuf.st_mode & 077)) return 1;
    char old[100];
    int old_status = read_small(p.directory, "panel.control", old, sizeof(old), 1);
    if (!old_status) {
        if (parse_control(old, &p.control)) { close(p.directory); return 1; }
    } else if (old_status != -2) { close(p.directory); return 1; }
    if (publish(&p)) { close(p.directory); return 1; }
    p.volume_starting = 1;
    for (unsigned i = 0; i < 3; ++i) volume_tick(&p);
    struct sigaction action; memset(&action, 0, sizeof(action)); action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) || sigaction(SIGHUP, &action, NULL)) return 1;
    if (atomic_file(&p, "panel.state", "running\n", 8)) return 1;
    fprintf(stderr, "panel: bounded evdev/LED loop started\n");
    while (!stopping) {
        uint64_t ms = now_ms();
#ifdef S12_PANEL_TEST
        if (p.end_at && ms >= p.end_at) break;
#endif
        if (p.input < 0 && !p.input_failed && ms >= p.next_scan) {
            p.next_scan = ms + 5000;
            p.input = scan_input(&p); p.partial_size = 0; p.sync_dropped = 0; memset(p.held, 0, sizeof(p.held));
        }
        if (p.input >= 0 && !p.input_failed && !p.sync_dropped) {
            struct panel_control previous = p.control;
            (void)apply_action(&p, ring_flush(&p, ms), previous);
        }
        volume_tick(&p);
        led_tick(&p, ms, 0);
        struct pollfd fd = { .fd = p.input, .events = POLLIN, .revents = 0 };
        int ready = poll(p.input < 0 ? NULL : &fd, p.input < 0 ? 0 : 1, 50);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (fd.revents & POLLIN)) {
            unsigned char bytes[512]; ssize_t n = read(p.input, bytes, sizeof(bytes));
            if (n > 0) input_bytes(&p, bytes, (size_t)n);
            else if (!n || (errno != EAGAIN && errno != EINTR)) { close(p.input); p.input = -1; ring_reset(&p); }
        } else if (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) { close(p.input); p.input = -1; ring_reset(&p); }
    }
    led_tick(&p, now_ms(), 1);
    (void)atomic_file(&p, "panel.state", "stopped\n", 8);
    if (p.input >= 0) close(p.input);
    close(p.directory); return 0;
}
