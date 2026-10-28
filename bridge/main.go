// Local transport adapter for Xiaozhi WebSocket protocol v1.
// Audio/JSON frames pass unchanged. This is not an ASR, LLM or TTS service.
package main

import (
	"context"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httputil"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type Config struct {
	Listen            string `json:"listen"`
	UpstreamURL       string `json:"upstream_url"`
	DeviceID          string `json:"device_id"`
	LocalTokenFile    string `json:"local_token_file"`
	UpstreamTokenFile string `json:"upstream_token_file"`
	CAFile            string `json:"ca_file"`
}

func privateFile(path string, limit int64) ([]byte, error) {
	if !filepath.IsAbs(path) || filepath.Clean(path) != path {
		return nil, errors.New("private path must be absolute and canonical")
	}
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, errors.New("private file unavailable")
	}
	f := os.NewFile(uintptr(fd), path)
	defer f.Close()
	s, err := f.Stat()
	if err != nil || !s.Mode().IsRegular() || s.Mode().Perm() != 0600 || s.Size() < 1 || s.Size() > limit {
		return nil, errors.New("private file must be owned regular 0600 and bounded")
	}
	meta, ok := s.Sys().(*syscall.Stat_t)
	if !ok || int(meta.Uid) != os.Geteuid() || meta.Nlink != 1 {
		return nil, errors.New("private file owner or links invalid")
	}
	b, err := io.ReadAll(io.LimitReader(f, limit+1))
	if err != nil || int64(len(b)) != s.Size() {
		return nil, errors.New("private file changed or unreadable")
	}
	return b, nil
}

func readToken(path string) (string, error) {
	b, err := privateFile(path, 512)
	if err != nil {
		return "", err
	}
	s := strings.TrimSuffix(strings.TrimSuffix(string(b), "\n"), "\r")
	if len(s) < 1 || len(s) > 500 || strings.TrimSpace(s) != s {
		return "", errors.New("token format invalid")
	}
	for _, c := range s {
		if c < 33 || c > 126 {
			return "", errors.New("token header bytes invalid")
		}
	}
	return s, nil
}

func loadConfig(path string) (Config, error) {
	var c Config
	b, err := privateFile(path, 8192)
	if err != nil {
		return c, err
	}
	d := json.NewDecoder(strings.NewReader(string(b)))
	d.DisallowUnknownFields()
	if d.Decode(&c) != nil || d.Decode(new(any)) != io.EOF {
		return c, errors.New("invalid config JSON")
	}
	return c, nil
}

func endpoint(c Config) (*url.URL, error) {
	if c.Listen != "127.0.0.1:18080" {
		return nil, errors.New("listen must be 127.0.0.1:18080")
	}
	mac, err := net.ParseMAC(c.DeviceID)
	if err != nil || len(mac) != 6 || strings.ToLower(c.DeviceID) != mac.String() {
		return nil, errors.New("device_id must be canonical six-byte MAC")
	}
	u, err := url.Parse(c.UpstreamURL)
	if err != nil || len(c.UpstreamURL) > 4096 || u.User != nil || u.Hostname() == "" || u.Fragment != "" || u.Opaque != "" || u.Path == "" {
		return nil, errors.New("invalid configured WebSocket endpoint")
	}
	for _, r := range c.UpstreamURL {
		if r <= 32 || r == 127 {
			return nil, errors.New("endpoint contains control or whitespace")
		}
	}
	if port := u.Port(); port != "" {
		n, e := strconv.Atoi(port)
		if e != nil || n < 1 || n > 65535 {
			return nil, errors.New("endpoint port invalid")
		}
	} else if strings.HasSuffix(u.Host, ":") {
		return nil, errors.New("endpoint port empty")
	}
	switch u.Scheme {
	case "wss":
		u.Scheme = "https"
	case "ws":
		// Cleartext is only available to an explicit private numeric address.
		// No DNS name can resolve outside that boundary after validation.
		ip, e := netip.ParseAddr(u.Hostname())
		if e != nil || !(ip.IsLoopback() || ip.IsPrivate()) || ip.Is4In6() {
			return nil, errors.New("ws requires private numeric IP; use wss for public servers")
		}
		u.Scheme = "http"
	default:
		return nil, errors.New("upstream_url requires ws or wss")
	}
	return u, nil
}

func bearer(token string) string {
	if strings.HasPrefix(token, "Bearer ") {
		return token
	}
	return "Bearer " + token
}

func newHandler(c Config) (http.Handler, error) {
	target, err := endpoint(c)
	if err != nil {
		return nil, err
	}
	if _, err = readToken(c.LocalTokenFile); err != nil {
		return nil, errors.New("local authentication token unavailable")
	}
	if c.UpstreamTokenFile != "" {
		if _, err = readToken(c.UpstreamTokenFile); err != nil {
			return nil, errors.New("upstream authentication token unavailable")
		}
	}
	roots, _ := x509.SystemCertPool()
	if roots == nil {
		roots = x509.NewCertPool()
	}
	if c.CAFile != "" {
		f, e := os.Open(c.CAFile)
		if e != nil {
			return nil, errors.New("CA bundle unavailable")
		}
		pem, e := io.ReadAll(io.LimitReader(f, (2<<20)+1))
		f.Close()
		if e != nil || len(pem) > 2<<20 || !roots.AppendCertsFromPEM(pem) {
			return nil, errors.New("CA bundle invalid")
		}
	}
	transport := &http.Transport{
		Proxy:               nil,
		DialContext:         (&net.Dialer{Timeout: 10 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
		TLSClientConfig:     &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12},
		TLSHandshakeTimeout: 10 * time.Second, ResponseHeaderTimeout: 15 * time.Second,
		IdleConnTimeout: 60 * time.Second, MaxIdleConnsPerHost: 2, ForceAttemptHTTP2: false,
	}
	proxy := &httputil.ReverseProxy{
		Transport: transport,
		ErrorLog:  log.New(io.Discard, "", 0), // Never log a URL query or credentials.
		Rewrite: func(p *httputil.ProxyRequest) {
			clean := make(http.Header)
			for _, key := range []string{"Client-Id", "Protocol-Version", "Sec-WebSocket-Key", "Sec-WebSocket-Version", "Sec-WebSocket-Protocol"} {
				for _, v := range p.In.Header.Values(key) {
					clean.Add(key, v)
				}
			}
			clean.Set("Connection", "Upgrade")
			clean.Set("Upgrade", "websocket")
			clean.Set("Device-Id", c.DeviceID)
			p.Out.Header = clean
			p.Out.URL = &url.URL{Scheme: target.Scheme, Host: target.Host, Path: target.Path, RawPath: target.RawPath, RawQuery: target.RawQuery}
			p.Out.Host = target.Host
			if c.UpstreamTokenFile != "" {
				// ServeHTTP reads and verifies this file before entering the proxy.
				p.Out.Header.Set("Authorization", bearer(p.In.Context().Value(remoteTokenKey{}).(string)))
			}
		},
		ModifyResponse: func(r *http.Response) error {
			if r.StatusCode >= 300 && r.StatusCode <= 399 {
				return errors.New("redirect rejected")
			}
			return nil
		},
		ErrorHandler: func(w http.ResponseWriter, _ *http.Request, _ error) {
			http.Error(w, "upstream transport unavailable", http.StatusBadGateway)
		},
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host, _, e := net.SplitHostPort(r.RemoteAddr)
		ip := net.ParseIP(host)
		if e != nil || ip == nil || !ip.IsLoopback() {
			http.Error(w, "loopback only", 403)
			return
		}
		if r.Method != "GET" || r.URL.Path != "/xiaozhi/v1/" || r.URL.RawPath != "" || r.URL.RawQuery != "" || r.URL.ForceQuery || r.Header.Get("Origin") != "" || r.ContentLength != 0 || len(r.TransferEncoding) != 0 {
			http.Error(w, "fixed WebSocket route only", 400)
			return
		}
		if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") || !headerToken(r.Header.Get("Connection"), "upgrade") || r.Header.Get("Sec-WebSocket-Version") != "13" || r.Header.Get("Protocol-Version") != "1" || r.Header.Get("Client-Id") == "" || strings.ToLower(r.Header.Get("Device-Id")) != c.DeviceID {
			http.Error(w, "invalid Xiaozhi v1 upgrade", 400)
			return
		}
		local, e := readToken(c.LocalTokenFile)
		if e != nil {
			http.Error(w, "local authentication unavailable", 503)
			return
		}
		if subtle.ConstantTimeCompare([]byte(r.Header.Get("Authorization")), []byte(bearer(local))) != 1 {
			http.Error(w, "authentication required", 401)
			return
		}
		if c.UpstreamTokenFile != "" {
			remote, e := readToken(c.UpstreamTokenFile)
			if e != nil {
				http.Error(w, "upstream authentication unavailable", 503)
				return
			}
			r = r.WithContext(context.WithValue(r.Context(), remoteTokenKey{}, remote))
		}
		proxy.ServeHTTP(w, r)
	}), nil
}

type remoteTokenKey struct{}

func headerToken(value, token string) bool {
	for _, item := range strings.Split(value, ",") {
		if strings.EqualFold(strings.TrimSpace(item), token) {
			return true
		}
	}
	return false
}

func main() {
	path := flag.String("config", "", "owned 0600 server configuration")
	check := flag.Bool("check", false, "validate configuration without listening or connecting")
	syncTime := flag.Bool("sync-time", false, "S12 init only: bounded background SNTP with release-time floor")
	flag.Parse()
	if *path == "" || flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "usage: bridge --config PRIVATE_FILE [--check]")
		os.Exit(2)
	}
	c, err := loadConfig(*path)
	if err != nil {
		fmt.Fprintln(os.Stderr, "bridge: configuration unavailable or invalid")
		os.Exit(1)
	}
	h, err := newHandler(c)
	if err != nil {
		fmt.Fprintln(os.Stderr, "bridge: endpoint, local credential or trust configuration invalid")
		os.Exit(1)
	}
	if *check {
		fmt.Println("bridge configuration valid; no connection started")
		return
	}
	if *syncTime {
		if err := startClockSync(); err != nil {
			fmt.Fprintln(os.Stderr, "bridge: S12 runtime clock gate refused")
			os.Exit(1)
		}
	}
	s := &http.Server{Addr: c.Listen, Handler: h, ReadHeaderTimeout: 5 * time.Second, MaxHeaderBytes: 8192, ErrorLog: log.New(io.Discard, "", 0)}
	if err := s.ListenAndServe(); err != nil {
		fmt.Fprintln(os.Stderr, "bridge: stopped")
		os.Exit(1)
	}
}
