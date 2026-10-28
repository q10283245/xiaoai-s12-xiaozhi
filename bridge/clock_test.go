package main

import (
	"encoding/binary"
	"testing"
	"time"
)

func TestClockReplyBounds(t *testing.T) {
	build := int64(1793000000)
	nonce := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	valid := make([]byte, 48)
	valid[0] = 0x24
	valid[1] = 2
	copy(valid[24:32], nonce)
	binary.BigEndian.PutUint64(valid[40:48], uint64(build+ntpEpoch+600)<<32)
	got, err := validateTimeReply(valid, nonce, build)
	if err != nil || !got.Equal(time.Unix(build+600, 0)) {
		t.Fatal(got, err)
	}
	tests := []struct {
		name   string
		mutate func([]byte)
	}{
		{"leap_unsynchronized", func(b []byte) { b[0] |= 0xc0 }},
		{"client_mode", func(b []byte) { b[0] = 0x23 }},
		{"stratum_zero", func(b []byte) { b[1] = 0 }},
		{"stratum_invalid", func(b []byte) { b[1] = 16 }},
		{"nonce", func(b []byte) { b[24] ^= 1 }},
		{"zero", func(b []byte) {
			for i := 40; i < 48; i++ {
				b[i] = 0
			}
		}},
		{"before_build", func(b []byte) { binary.BigEndian.PutUint64(b[40:48], uint64(build+ntpEpoch-1)<<32) }},
		{"after_lifetime", func(b []byte) { binary.BigEndian.PutUint64(b[40:48], uint64(build+ntpEpoch+6*366*86400)<<32) }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			b := append([]byte(nil), valid...)
			test.mutate(b)
			if _, err := validateTimeReply(b, nonce, build); err == nil {
				t.Fatal("invalid reply accepted")
			}
		})
	}
	if _, err := validateTimeReply(valid[:40], nonce, build); err == nil {
		t.Fatal("short response accepted")
	}
}
