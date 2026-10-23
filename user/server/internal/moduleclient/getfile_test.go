package moduleclient

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"
	"time"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

func readGetfileRequest(r io.Reader) (string, error) {
	var request [wire.HeaderSize]byte
	if _, err := io.ReadFull(r, request[:]); err != nil {
		return "", err
	}
	h, err := wire.DecodeHeader(request[:])
	if err != nil || h.Command != wire.CommandGetfile || h.Flags != 0 || h.Status != 0 || h.PayloadSize > wire.GetfileMaxPathLength {
		return "", fmt.Errorf("unexpected GETFILE header: %+v, %v", h, err)
	}
	body := make([]byte, int(h.PayloadSize))
	_, err = io.ReadFull(r, body)
	return string(body), err
}

func TestGetfilePathRequestAndSessionReuse(t *testing.T) {
	c, peer := newHandshakedClient(t, 1)
	defer c.Close()
	defer peer.Close()
	_ = peer.SetDeadline(time.Now().Add(2 * time.Second))
	paths := []string{"/etc/hostname", "/tmp/a  b/שלום", "/" + strings.Repeat("x", wire.GetfileMaxPathLength-1)}
	go func() {
		defer peer.Close()
		for _, path := range paths {
			got, err := readGetfileRequest(peer)
			if err != nil || got != path {
				t.Errorf("request = %q, %v; want %q", got, err, path)
				return
			}
			response := wire.Header{Command: wire.CommandGetfile, PayloadSize: uint64(len(path))}.Encode()
			peer.Write(response[:])
			peer.Write([]byte(path[:1]))
			peer.Write([]byte(path[1:]))
		}
	}()
	for _, path := range paths {
		data, err := getfileBytes(c, context.Background(), path)
		if err != nil || string(data) != path {
			t.Fatalf("Getfile = %q, %v", data, err)
		}
	}
}

func TestGetfileCommandErrorsKeepSessionUsable(t *testing.T) {
	c, peer := newHandshakedClient(t, 1)
	defer c.Close()
	defer peer.Close()
	_ = peer.SetDeadline(time.Now().Add(2 * time.Second))
	go func() {
		defer peer.Close()
		for status := wire.StatusInvalidArgument; status <= wire.StatusInternalError; status++ {
			if _, err := readGetfileRequest(peer); err != nil {
				t.Error(err)
				return
			}
			h := wire.Header{Command: wire.CommandGetfile, Status: status}.Encode()
			peer.Write(h[:])
		}
		if _, err := readGetfileRequest(peer); err != nil {
			t.Error(err)
			return
		}
		h := wire.Header{Command: wire.CommandGetfile, PayloadSize: 3}.Encode()
		peer.Write(h[:])
		peer.Write([]byte("/ok"))
	}()
	for status := wire.StatusInvalidArgument; status <= wire.StatusInternalError; status++ {
		_, err := getfileBytes(c, context.Background(), "/rejected")
		var rejection *CommandError
		if !errors.As(err, &rejection) || rejection.Status != status || c.Broken() {
			t.Fatalf("status=%d: error=%v, broken=%v", status, err, c.Broken())
		}
	}
	data, err := getfileBytes(c, context.Background(), "/ok")
	if err != nil || string(data) != "/ok" {
		t.Fatalf("request after rejection: %q, %v", data, err)
	}
}

func TestGetfileRejectsInvalidResponses(t *testing.T) {
	for _, test := range []struct {
		name   string
		header wire.Header
		body   string
	}{
		{"stream", wire.Header{Command: wire.CommandGetfile, Flags: wire.FlagStream}, ""},
		{"oversize", wire.Header{Command: wire.CommandGetfile, PayloadSize: 1 << 63}, ""},
		{"wrong command", wire.Header{Command: wire.CommandEcho}, ""},
		{"error with payload", wire.Header{Command: wire.CommandGetfile, Status: wire.StatusInvalidArgument, PayloadSize: 1}, ""},
		{"error with stream", wire.Header{Command: wire.CommandGetfile, Status: wire.StatusInvalidArgument, Flags: wire.FlagStream}, ""},
		{"unknown status", wire.Header{Command: wire.CommandGetfile, Status: 99}, ""},
		{"unknown flags", wire.Header{Command: wire.CommandGetfile, Flags: 2}, ""},
		{"truncated", wire.Header{Command: wire.CommandGetfile, PayloadSize: 5}, "He"},
	} {
		t.Run(test.name, func(t *testing.T) {
			c, peer := newHandshakedClient(t, 1)
			defer c.Close()
			defer peer.Close()
			_ = peer.SetDeadline(time.Now().Add(2 * time.Second))
			go func() {
				defer peer.Close()
				if _, err := readGetfileRequest(peer); err != nil {
					t.Error(err)
					return
				}
				response := test.header.Encode()
				peer.Write(response[:])
				if test.body != "" {
					peer.Write([]byte(test.body))
				}
			}()
			if _, err := getfileBytes(c, context.Background(), "/test"); err == nil {
				t.Fatal("expected error")
			}
			if _, err := getfileBytes(c, context.Background(), "/test"); !errors.Is(err, ErrSessionBroken) {
				t.Fatalf("session should be broken: %v", err)
			}
		})
	}
}

func TestGetfileRejectsInvalidInputBeforeWriting(t *testing.T) {
	c, peer := newHandshakedClient(t, 1)
	defer c.Close()
	peer.Close() // Any attempt to send would fail and break the session.
	for _, path := range []string{"", "relative", "/a\x00b", "/" + strings.Repeat("a", 4096), "/" + strings.Repeat("é", 2048)} {
		_, err := getfileBytes(c, context.Background(), path)
		if !errors.Is(err, ErrInvalidGetfilePath) || c.Broken() {
			t.Fatalf("invalid path: %v, broken=%v", err, c.Broken())
		}
	}
}

func TestGetfileBusyAndCanceled(t *testing.T) {
	c, peer := newHandshakedClient(t, 1)
	defer c.Close()
	defer peer.Close()
	c.cmdMu.Lock()
	_, err := getfileBytes(c, context.Background(), "/test")
	c.cmdMu.Unlock()
	if !errors.Is(err, ErrSessionBusy) {
		t.Fatalf("busy: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := getfileBytes(c, ctx, "/test"); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancel: %v", err)
	}
	if c.Broken() {
		t.Fatal("busy/canceled request broke session")
	}
}

// Used only by the small fixture tests; production callers stream the body.
func getfileBytes(c *ModuleClient, ctx context.Context, path string) ([]byte, error) {
	download, err := c.Getfile(ctx, path)
	if err != nil {
		return nil, err
	}
	defer download.Close()
	return io.ReadAll(download)
}

func TestGetfileHeaderDoesNotBufferLargeFileAndHoldsLock(t *testing.T) {
	c, peer := newHandshakedClient(t, 1)
	defer c.Close()
	defer peer.Close()
	peer.SetDeadline(time.Now().Add(2 * time.Second))
	go func() {
		if _, err := readGetfileRequest(peer); err != nil {
			t.Error(err)
			return
		}
		h := wire.Header{Command: wire.CommandGetfile, PayloadSize: 1 << 40}.Encode()
		peer.Write(h[:]) // No body: Getfile must return without waiting for it.
	}()
	body, err := c.Getfile(context.Background(), "/large")
	if err != nil {
		t.Fatal(err)
	}
	if body.Size != 1<<40 || !c.Busy() {
		t.Fatal("size lost or lock released")
	}
	if _, err := c.Getfile(context.Background(), "/second"); !errors.Is(err, ErrSessionBusy) {
		t.Fatalf("overlap: %v", err)
	}
	body.Close()
	body.Close()
	if !c.Broken() || c.Busy() {
		t.Fatal("abandoned transfer must break connection and release lock")
	}
}

func TestGetfileCancellationInterruptsHeaderAndBodyReads(t *testing.T) {
	for _, phase := range []string{"header", "body"} {
		t.Run(phase, func(t *testing.T) {
			c, peer := newHandshakedClient(t, 1)
			defer c.Close()
			defer peer.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			ready := make(chan struct{})
			go func() {
				defer close(ready)
				if _, err := readGetfileRequest(peer); err != nil {
					t.Error(err)
					return
				}
				if phase == "body" {
					h := wire.Header{Command: wire.CommandGetfile, PayloadSize: 100}.Encode()
					peer.Write(h[:])
				}
			}()
			finished := make(chan error, 1)
			go func() {
				body, err := c.Getfile(ctx, "/blocked")
				if err == nil {
					_, err = io.Copy(io.Discard, body)
					body.Close()
				}
				finished <- err
			}()
			<-ready
			cancel()
			select {
			case err := <-finished:
				if err == nil || !c.Broken() || c.Busy() {
					t.Fatalf("cancel: %v broken=%v busy=%v", err, c.Broken(), c.Busy())
				}
			case <-time.After(2 * time.Second):
				t.Fatal("cancellation left transfer blocked")
			}
		})
	}
}
