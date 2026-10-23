package server

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/GhostDog788/Mapple/user/api"
	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

func TestGetfileHTTPToModule(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	done := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			done <- err
			return
		}
		defer conn.Close()
		_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
		for _, step := range []struct {
			command wire.Command
			path    string
			status  wire.Status
		}{
			{wire.CommandInfo, "", wire.StatusOK},
			{wire.CommandGetfile, "/tmp/a  b/שלום", wire.StatusOK},
			{wire.CommandGetfile, "/rejected", wire.StatusInvalidArgument},
			{wire.CommandGetfile, "/denied", wire.StatusPermissionDenied},
			{wire.CommandGetfile, "/etc/hostname", wire.StatusOK},
		} {
			command := step.command
			var buf [wire.HeaderSize]byte
			if _, err := io.ReadFull(conn, buf[:]); err != nil {
				done <- err
				return
			}
			h, err := wire.DecodeHeader(buf[:])
			if err != nil || h != (wire.Header{Command: command, PayloadSize: uint64(len(step.path))}) {
				done <- fmt.Errorf("unexpected request: %+v, %v", h, err)
				return
			}
			path := make([]byte, len(step.path))
			if _, err := io.ReadFull(conn, path); err != nil || string(path) != step.path {
				done <- fmt.Errorf("request path: %q, %v", path, err)
				return
			}
			body := path
			if step.status != wire.StatusOK {
				body = nil
			}
			if command == wire.CommandInfo {
				body = make([]byte, wire.InfoResponseSize)
				binary.BigEndian.PutUint64(body[:8], 1)
				binary.BigEndian.PutUint32(body[8:12], 8)
				binary.BigEndian.PutUint64(body[16:], 1<<wire.CommandGetfile)
			}
			response := wire.Header{Command: command, Status: step.status, PayloadSize: uint64(len(body))}.Encode()
			if _, err := conn.Write(response[:]); err != nil {
				done <- err
				return
			}
			if _, err := conn.Write(body); err != nil {
				done <- err
				return
			}
		}
		done <- nil
	}()
	sessions := NewSessionManager(time.Second, time.Minute)
	defer sessions.Stop()
	defer sessions.CloseAll()
	cfg := &Config{Identities: []Identity{{Name: "test", TokenSHA256: sha256.Sum256([]byte("test-token"))}}}
	httpServer := httptest.NewServer(New(cfg, sessions, slog.New(slog.NewTextHandler(io.Discard, nil))).Handler())
	defer httpServer.Close()
	client, err := api.NewClientWithResponses(httpServer.URL+"/v1", api.WithRequestEditorFn(func(_ context.Context, req *http.Request) error {
		req.Header.Set("Authorization", "Bearer test-token")
		return nil
	}))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	host, port := "127.0.0.1", listener.Addr().(*net.TCPAddr).Port
	created, err := client.PostSessionsWithResponse(ctx, api.Destination{Type: api.Tcp, Host: &host, Port: &port})
	if err != nil {
		t.Fatal(err)
	}
	if created.JSON201 == nil {
		t.Fatalf("create: %s", created.Body)
	}
	if !slices.Contains(created.JSON201.Info.SupportedCommands, "getfile") {
		t.Fatal("getfile capability missing")
	}
	// Invalid local input must not reach the module or poison the lease.
	for _, path := range []string{"", "relative", "/a\x00b", "/" + strings.Repeat("x", 4096), "/" + strings.Repeat("é", 2048)} {
		result, err := client.PostSessionsIdCommandsGetfileWithResponse(ctx, created.JSON201.Id, api.GetfileRequest{Path: path})
		if err != nil || result.JSON400 == nil {
			t.Fatalf("invalid input: %+v, %v", result, err)
		}
	}
	for _, test := range []struct {
		path   string
		status int
	}{
		{"/tmp/a  b/שלום", 200}, {"/rejected", 400}, {"/denied", 422}, {"/etc/hostname", 200},
	} {
		result, err := client.PostSessionsIdCommandsGetfile(ctx, created.JSON201.Id, api.GetfileRequest{Path: test.path})
		if err != nil {
			t.Fatal(err)
		}
		body, readErr := io.ReadAll(result.Body)
		result.Body.Close()
		if readErr != nil {
			t.Fatal(readErr)
		}
		if result.StatusCode != test.status {
			t.Fatalf("getfile %q: %s", test.path, body)
		}
		if test.status == 200 && (string(body) != test.path || result.ContentLength != int64(len(test.path)) || result.Header.Get("Content-Type") != "application/octet-stream") {
			t.Fatalf("getfile: %s", body)
		}
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	missing, err := client.PostSessionsIdCommandsGetfileWithResponse(ctx, "missing", api.GetfileRequest{Path: "/test"})
	if err != nil || missing.StatusCode() != 404 {
		t.Fatalf("missing: %v, %v", missing, err)
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/sessions/missing/commands/getfile", nil)
	recorder := httptest.NewRecorder()
	New(cfg, sessions, slog.Default()).Handler().ServeHTTP(recorder, req)
	if recorder.Code != 401 {
		t.Fatalf("unauthenticated status: %d", recorder.Code)
	}
}

// A gated module proves the HTTP response is delivered before the whole file
// exists. A second request then exercises truncation after HTTP headers commit.
func TestGetfileStreamsBeforeEOFAndAbortsTruncatedHTTP(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	release := make(chan struct{})
	var releaseOnce sync.Once
	unblock := func() { releaseOnce.Do(func() { close(release) }) }
	defer unblock()
	done := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			done <- err
			return
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(5 * time.Second))
		for i := 0; i < 3; i++ {
			var raw [wire.HeaderSize]byte
			if _, err := io.ReadFull(conn, raw[:]); err != nil {
				done <- err
				return
			}
			h, err := wire.DecodeHeader(raw[:])
			if err != nil {
				done <- err
				return
			}
			if h.PayloadSize > 4096 {
				done <- fmt.Errorf("oversize request")
				return
			}
			path := make([]byte, h.PayloadSize)
			if _, err := io.ReadFull(conn, path); err != nil {
				done <- err
				return
			}
			if i == 0 {
				if h.Command != wire.CommandInfo {
					done <- fmt.Errorf("missing handshake")
					return
				}
				body := make([]byte, wire.InfoResponseSize)
				header := wire.Header{Command: wire.CommandInfo, PayloadSize: uint64(len(body))}.Encode()
				conn.Write(header[:])
				conn.Write(body)
				continue
			}
			if h.Command != wire.CommandGetfile || string(path) != "/file" {
				done <- fmt.Errorf("bad request")
				return
			}
			header := wire.Header{Command: wire.CommandGetfile, PayloadSize: 128 * 1024}.Encode()
			conn.Write(header[:])
			chunk := bytes.Repeat([]byte{0, 1, 255, 2}, 8192)
			if _, err := conn.Write(chunk); err != nil {
				done <- err
				return
			}
			if i == 2 {
				break
			} // Advertised 128 KiB; close after 32 KiB.
			<-release
			for j := 0; j < 3; j++ {
				if _, err := conn.Write(chunk); err != nil {
					done <- err
					return
				}
			}
		}
		done <- nil
	}()
	sessions := NewSessionManager(time.Second, time.Minute)
	defer sessions.Stop()
	defer sessions.CloseAll()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	lease, err := sessions.Create(ctx, Destination{Type: DestinationTCP, Host: "127.0.0.1", Port: listener.Addr().(*net.TCPAddr).Port})
	if err != nil {
		t.Fatal(err)
	}
	cfg := &Config{Identities: []Identity{{Name: "test", TokenSHA256: sha256.Sum256([]byte("token"))}}}
	server := httptest.NewServer(New(cfg, sessions, slog.New(slog.NewTextHandler(io.Discard, nil))).Handler())
	defer server.Close()
	client, _ := api.NewClientWithResponses(server.URL+"/v1", api.WithRequestEditorFn(func(_ context.Context, r *http.Request) error {
		r.Header.Set("Authorization", "Bearer token")
		return nil
	}))
	response, err := client.PostSessionsIdCommandsGetfile(ctx, lease.ID, api.GetfileRequest{Path: "/file"})
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	first := make([]byte, 8192)
	if _, err := io.ReadFull(response.Body, first); err != nil {
		t.Fatal("no streaming data before module completion", err)
	}
	if !lease.Client.Busy() {
		t.Fatal("lease unlocked during transfer")
	}
	unblock()
	n, err := io.Copy(io.Discard, response.Body)
	response.Body.Close()
	if err != nil || n+int64(len(first)) != 128*1024 {
		t.Fatalf("full transfer: %d %v", n, err)
	}
	// Completion of the HTTP body can precede handler cleanup very briefly.
	for lease.Client.Busy() && ctx.Err() == nil {
		time.Sleep(time.Millisecond)
	}
	response, err = client.PostSessionsIdCommandsGetfile(ctx, lease.ID, api.GetfileRequest{Path: "/file"})
	if err != nil {
		t.Fatal(err)
	}
	n, err = io.Copy(io.Discard, response.Body)
	response.Body.Close()
	if err == nil || n >= response.ContentLength {
		t.Fatalf("truncation was accepted: %d %v", n, err)
	}
	if !lease.Client.Broken() {
		t.Fatal("truncated module session remained usable")
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
