package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/pem"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func configForTest(t *testing.T, url string) Config {
	t.Helper()
	p := filepath.Join(t.TempDir(), "local-token")
	if err := os.WriteFile(p, []byte("local-only-example-token"), 0600); err != nil {
		t.Fatal(err)
	}
	return Config{Listen: "127.0.0.1:18080", UpstreamURL: url, DeviceID: "02:00:00:00:00:01", LocalTokenFile: p}
}

func upgradeRequest() *http.Request {
	r := httptest.NewRequest("GET", "http://127.0.0.1/xiaozhi/v1/", nil)
	r.RemoteAddr = "127.0.0.1:12345"
	r.Header.Set("Connection", "Upgrade")
	r.Header.Set("Upgrade", "websocket")
	r.Header.Set("Sec-WebSocket-Version", "13")
	r.Header.Set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==")
	r.Header.Set("Protocol-Version", "1")
	r.Header.Set("Device-Id", "02:00:00:00:00:01")
	r.Header.Set("Client-Id", "test-client")
	r.Header.Set("Authorization", "Bearer local-only-example-token")
	return r
}

func TestEndpointBoundaries(t *testing.T) {
	for _, raw := range []string{"wss://example.com/xiaozhi/v1/", "ws://127.0.0.1:8000/ws", "ws://192.168.1.2/ws"} {
		c := configForTest(t, raw)
		if _, err := endpoint(c); err != nil {
			t.Fatal(raw, err)
		}
	}
	for _, raw := range []string{"https://example.com/ws", "ws://example.com/ws", "ws://8.8.8.8/ws", "wss://user:pass@example.com/ws", "wss://example.com/ws#fragment", "wss://example.com", "wss://example.com:0/ws", "wss://example.com:65536/ws", "wss://example.com:/ws"} {
		c := configForTest(t, raw)
		if _, err := endpoint(c); err == nil {
			t.Fatal("accepted", raw)
		}
	}
	c := configForTest(t, "wss://example.com/ws")
	c.Listen = "0.0.0.0:18080"
	if _, e := endpoint(c); e == nil {
		t.Fatal("public bind accepted")
	}
}

func TestRequestBoundaries(t *testing.T) {
	c := configForTest(t, "ws://127.0.0.1:1/ws")
	h, e := newHandler(c)
	if e != nil {
		t.Fatal(e)
	}
	tests := []struct {
		name   string
		change func(*http.Request)
		code   int
	}{
		{"peer", func(r *http.Request) { r.RemoteAddr = "192.168.1.2:10" }, 403},
		{"path", func(r *http.Request) { r.URL.Path = "/ws/other" }, 400},
		{"origin", func(r *http.Request) { r.Header.Set("Origin", "https://example.com") }, 400},
		{"query", func(r *http.Request) { r.URL.RawQuery = "target=other" }, 400},
		{"device", func(r *http.Request) { r.Header.Set("Device-Id", "02:00:00:00:00:02") }, 400},
		{"protocol", func(r *http.Request) { r.Header.Set("Protocol-Version", "3") }, 400},
		{"auth", func(r *http.Request) { r.Header.Set("Authorization", "Bearer wrong") }, 401},
		{"method", func(r *http.Request) { r.Method = "POST" }, 400},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			r := upgradeRequest()
			test.change(r)
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if w.Code != test.code {
				t.Fatalf("got %d", w.Code)
			}
		})
	}
}

func TestPrivateInputs(t *testing.T) {
	c := configForTest(t, "ws://127.0.0.1:1/ws")
	if err := os.Chmod(c.LocalTokenFile, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := newHandler(c); err == nil {
		t.Fatal("world-readable token accepted")
	}
	os.Chmod(c.LocalTokenFile, 0600)
	link := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(c.LocalTokenFile, link); err != nil {
		t.Fatal(err)
	}
	c.LocalTokenFile = link
	if _, err := newHandler(c); err == nil {
		t.Fatal("symlink token accepted")
	}
}

func TestWebSocketTLSAndCredentialSeparation(t *testing.T) {
	seen := make(chan http.Header, 1)
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/custom/ws" || r.URL.RawQuery != "model=test" {
			t.Error("configured upstream route changed")
		}
		seen <- r.Header.Clone()
		conn, rw, err := w.(http.Hijacker).Hijack()
		if err != nil {
			return
		}
		defer conn.Close()
		digest := sha1.Sum([]byte(r.Header.Get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
		fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", base64.StdEncoding.EncodeToString(digest[:]))
		rw.Flush()
		// Echo exact raw WebSocket bytes: transport must never parse or buffer
		// the Opus/JSON payload into an application-specific message queue.
		io.Copy(conn, rw)
	}))
	defer upstream.Close()
	c := configForTest(t, strings.Replace(upstream.URL, "https://", "wss://", 1)+"/custom/ws?model=test")
	ca := filepath.Join(t.TempDir(), "ca.pem")
	os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: upstream.Certificate().Raw}), 0600)
	c.CAFile = ca
	remote := filepath.Join(t.TempDir(), "remote-token")
	os.WriteFile(remote, []byte("remote-example-token"), 0600)
	c.UpstreamTokenFile = remote
	h, e := newHandler(c)
	if e != nil {
		t.Fatal(e)
	}
	local := httptest.NewServer(h)
	defer local.Close()
	conn, e := net.DialTimeout("tcp", strings.TrimPrefix(local.URL, "http://"), time.Second)
	if e != nil {
		t.Fatal(e)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	r := upgradeRequest()
	r.Host = strings.TrimPrefix(local.URL, "http://")
	r.URL.Scheme = ""
	r.URL.Host = ""
	r.Header.Set("Cookie", "private-cookie")
	r.Header.Set("X-Forwarded-Host", "evil.invalid")
	if e = r.Write(conn); e != nil {
		t.Fatal(e)
	}
	reader := bufio.NewReader(conn)
	resp, e := http.ReadResponse(reader, r)
	if e != nil || resp.StatusCode != 101 {
		t.Fatalf("upgrade=%v %v", resp, e)
	}
	frame := []byte{0x82, 0x84, 1, 2, 3, 4, 0x10 ^ 1, 0x20 ^ 2, 0x30 ^ 3, 0x40 ^ 4}
	conn.Write(frame)
	got := make([]byte, len(frame))
	if _, e = io.ReadFull(reader, got); e != nil || string(got) != string(frame) {
		t.Fatalf("binary frame changed: %v", e)
	}
	head := <-seen
	if head.Get("Authorization") != "Bearer remote-example-token" || head.Get("Device-Id") != c.DeviceID || head.Get("Client-Id") != "test-client" {
		t.Fatal("credential/identity mapping wrong")
	}
	for _, key := range []string{"Cookie", "X-Forwarded-Host", "X-Forwarded-For"} {
		if head.Get(key) != "" {
			t.Fatal("leaked", key)
		}
	}
}

func TestAnonymousServerOmitsLocalToken(t *testing.T) {
	seen := make(chan string, 1)
	u := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen <- r.Header.Get("Authorization")
		w.WriteHeader(401)
	}))
	defer u.Close()
	c := configForTest(t, strings.Replace(u.URL, "http://", "ws://", 1)+"/ws")
	h, e := newHandler(c)
	if e != nil {
		t.Fatal(e)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, upgradeRequest())
	if <-seen != "" {
		t.Fatal("local credential sent to anonymous server")
	}
}

func TestUntrustedTLSFailsClosed(t *testing.T) {
	u := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { t.Error("untrusted request reached application") }))
	defer u.Close()
	u.Config.ErrorLog = nil
	c := configForTest(t, strings.Replace(u.URL, "https://", "wss://", 1)+"/ws")
	h, e := newHandler(c)
	if e != nil {
		t.Fatal(e)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, upgradeRequest())
	if w.Code != 502 {
		t.Fatal(w.Code)
	}
}
