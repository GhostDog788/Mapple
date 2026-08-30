package moduleclient

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"testing"
	"time"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

func serveInfoOnce(t *testing.T, conn net.Conn, sessionID uint64) {
	t.Helper()
	req := make([]byte, wire.HeaderSize)
	if _, err := io.ReadFull(conn, req); err != nil {
		t.Errorf("server: read request: %v", err)
		return
	}
	if h, err := wire.DecodeHeader(req); err != nil || h.Command != wire.CommandInfo {
		t.Errorf("server: unexpected request %+v err=%v", h, err)
		return
	}
	resp := wire.Header{Command: wire.CommandInfo, PayloadSize: wire.InfoResponseSize}.Encode()
	if _, err := conn.Write(resp[:]); err != nil {
		t.Errorf("server: write response header: %v", err)
		return
	}
	var body [wire.InfoResponseSize]byte
	binary.BigEndian.PutUint64(body[0:8], sessionID)
	binary.BigEndian.PutUint32(body[8:12], 8)
	binary.BigEndian.PutUint64(body[16:24], 0b1110)
	if _, err := conn.Write(body[:]); err != nil {
		t.Errorf("server: write response body: %v", err)
	}
}

func newHandshakedClient(t *testing.T, sessionID uint64) (*ModuleClient, net.Conn) {
	t.Helper()
	clientSide, serverSide := net.Pipe()
	go serveInfoOnce(t, serverSide, sessionID)
	c, err := newClient(context.Background(), clientSide)
	if err != nil {
		t.Fatalf("newClient() error = %v", err)
	}
	return c, serverSide
}

func TestNewClient_Success(t *testing.T) {
	c, server := newHandshakedClient(t, 42)
	defer server.Close()
	defer c.Close()
}

func TestNewClient_BadHandshakeResponse(t *testing.T) {
	clientSide, serverSide := net.Pipe()
	go func() {
		buf := make([]byte, wire.HeaderSize)
		io.ReadFull(serverSide, buf)
		serverSide.Write([]byte("not a valid header!!!!!"))
		serverSide.Close()
	}()
	if _, err := newClient(context.Background(), clientSide); err == nil {
		t.Fatal("newClient() with garbage handshake response: want error, got nil")
	}
}

func TestClient_EchoRoundTrip(t *testing.T) {
	c, server := newHandshakedClient(t, 1)
	defer server.Close()
	defer c.Close()

	payload := []byte("hello")
	go func() {
		req := make([]byte, wire.HeaderSize)
		io.ReadFull(server, req)
		h, _ := wire.DecodeHeader(req)
		body := make([]byte, h.PayloadSize)
		io.ReadFull(server, body)
		resp := wire.Header{Command: wire.CommandEcho, PayloadSize: uint64(len(body))}.Encode()
		server.Write(resp[:])
		server.Write(body)
	}()

	got, err := c.Echo(context.Background(), payload)
	if err != nil {
		t.Fatalf("Echo() error = %v", err)
	}
	if string(got) != string(payload) {
		t.Fatalf("Echo() = %q, want %q", got, payload)
	}
}

func TestClient_BusyThenBrokenAfterForceClose(t *testing.T) {
	c, server := newHandshakedClient(t, 1)
	defer server.Close()

	started := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		close(started)
		_, err := c.Echo(context.Background(), []byte("hi")) // server never reads: blocks on Write
		done <- err
	}()
	<-started
	time.Sleep(100 * time.Millisecond) // let the goroutine acquire cmdMu and block

	if _, err := c.Info(context.Background()); !errors.Is(err, ErrSessionBusy) {
		t.Fatalf("Info() while busy = %v, want ErrSessionBusy", err)
	}

	if err := c.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	select {
	case err := <-done:
		if err == nil {
			t.Fatal("blocked Echo() returned nil error after Close()")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Close() did not unblock the pending Echo()")
	}

	if _, err := c.Info(context.Background()); !errors.Is(err, ErrSessionBroken) {
		t.Fatalf("Info() after Close() = %v, want ErrSessionBroken", err)
	}
}
