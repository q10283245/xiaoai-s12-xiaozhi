package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// SNTP is a best-effort clock source, not authenticated time. Replies are only
// accepted within the explicitly bounded release lifetime. TLS verification is
// never bypassed if DNS/NTP are unavailable. No voice/model service is involved.
var timeSources = []string{"ntp.aliyun.com:123", "pool.ntp.org:123"}

const ntpEpoch = 2208988800

func validateTimeReply(reply, nonce []byte, buildUnix int64) (time.Time, error) {
	if len(reply) < 48 || len(nonce) != 8 || reply[0]>>6 == 3 || reply[0]&7 != 4 || (reply[0]>>3)&7 < 3 || (reply[0]>>3)&7 > 4 || reply[1] < 1 || reply[1] > 15 || !bytes.Equal(reply[24:32], nonce) {
		return time.Time{}, errors.New("invalid SNTP response")
	}
	raw := binary.BigEndian.Uint64(reply[40:48])
	if raw == 0 {
		return time.Time{}, errors.New("zero SNTP timestamp")
	}
	seconds := int64(raw>>32) - ntpEpoch
	nanos := int64(raw&0xffffffff) * 1000000000 >> 32
	value := time.Unix(seconds, nanos)
	if value.Before(time.Unix(buildUnix, 0)) || value.After(time.Unix(buildUnix, 0).AddDate(5, 0, 0)) || seconds > 2147483647 {
		return time.Time{}, errors.New("SNTP outside signed ARM release lifetime")
	}
	return value, nil
}

func queryTime(ctx context.Context, source string, buildUnix int64) (time.Time, error) {
	packet := make([]byte, 48)
	packet[0] = 0x23
	if _, err := rand.Read(packet[40:48]); err != nil {
		return time.Time{}, err
	}
	conn, err := (&net.Dialer{}).DialContext(ctx, "udp", source)
	if err != nil {
		return time.Time{}, err
	}
	defer conn.Close()
	if deadline, ok := ctx.Deadline(); ok {
		conn.SetDeadline(deadline)
	}
	started := time.Now()
	if _, err = conn.Write(packet); err != nil {
		return time.Time{}, err
	}
	reply := make([]byte, 512)
	n, err := conn.Read(reply)
	if err != nil {
		return time.Time{}, err
	}
	value, err := validateTimeReply(reply[:n], packet[40:48], buildUnix)
	if err != nil {
		return time.Time{}, err
	}
	return value.Add(time.Since(started) / 2), nil
}

func startClockSync() error {
	// This flag must never change a contributor's workstation clock during
	// tests or --check. The image's private PID1 marker is required in addition
	// to OS privilege, before any query or setter is started.
	if os.Geteuid() != 0 {
		return errors.New("root S12 init required")
	}
	for path, wanted := range map[string]string{"/run/s12/supervisor.tag": "S12_CUSTOM_RUNTIME_V1\n", "/run/s12/supervisor.pid": "1\n"} {
		b, e := privateFile(path, 128)
		if e != nil || string(b) != wanted {
			return errors.New("S12 supervisor marker required")
		}
	}
	raw, err := privateFile("/s12/build-unix", 32)
	if err != nil {
		return err
	}
	buildUnix, err := strconv.ParseInt(strings.TrimSpace(string(raw)), 10, 64)
	if err != nil || buildUnix < 1704067200 || buildUnix > 2147483647 {
		return errors.New("release clock bound invalid")
	}
	go func() {
		retry := 5 * time.Second
		for {
			// The init starts the local bridge before WPA/DHCP. Avoid spending
			// the first request timeout while no link exists; check promptly
			// after the image's network supervisor marks the connection ready.
			state, e := os.ReadFile("/run/s12/network.state")
			if e != nil || string(state) != "ready\n" {
				time.Sleep(5 * time.Second)
				continue
			}
			success := false
			for _, source := range timeSources {
				ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
				value, e := queryTime(ctx, source, buildUnix)
				cancel()
				if e == nil {
					tv := syscall.NsecToTimeval(value.UnixNano())
					if syscall.Settimeofday(&tv) == nil {
						success = true
						break
					}
				}
			}
			if success {
				fmt.Fprintln(os.Stderr, "clock synchronized; TLS validation enabled")
				retry = 5 * time.Second
				time.Sleep(6 * time.Hour)
			} else {
				fmt.Fprintln(os.Stderr, "clock unavailable; retrying with TLS validation enabled")
				time.Sleep(retry)
				if retry < time.Minute {
					retry *= 2
					if retry > time.Minute {
						retry = time.Minute
					}
				}
			}
		}
	}()
	return nil
}
