//go:build integration

package moduleclient

import (
	"context"
	"os"
	"strconv"
	"testing"
)

// go test -tags=integration ./... against a real running mapple.ko, e.g.:
//   MAPPLE_TEST_TCP_HOST=192.168.122.77 MAPPLE_TEST_TCP_PORT=4444 go test -tags=integration ./...
func testTCPTarget(t *testing.T) (string, uint16) {
	t.Helper()
	host := os.Getenv("MAPPLE_TEST_TCP_HOST")
	if host == "" {
		t.Skip("MAPPLE_TEST_TCP_HOST not set; skipping integration test")
	}
	portStr := os.Getenv("MAPPLE_TEST_TCP_PORT")
	if portStr == "" {
		portStr = "4444"
	}
	port, err := strconv.ParseUint(portStr, 10, 16)
	if err != nil {
		t.Fatalf("MAPPLE_TEST_TCP_PORT=%q: %v", portStr, err)
	}
	return host, uint16(port)
}

func TestIntegration_Info(t *testing.T) {
	host, port := testTCPTarget(t)
	c, err := NewTCP(context.Background(), host, port)
	if err != nil {
		t.Fatalf("NewTCP() error = %v", err)
	}
	defer c.Close()

	info, err := c.Info(context.Background())
	if err != nil {
		t.Fatalf("Info() error = %v", err)
	}
	if info.MaxSessions != 8 {
		t.Errorf("MaxSessions = %d, want 8", info.MaxSessions)
	}
	if info.SupportedCommands&0b1110 != 0b1110 {
		t.Errorf("SupportedCommands = %#b, want at least 0b1110 (echo|pslist|info)", info.SupportedCommands)
	}
	if info.SessionID == 0 {
		t.Error("SessionID = 0, want nonzero")
	}
}

func TestIntegration_CharDev(t *testing.T) {
	path := os.Getenv("MAPPLE_TEST_CHARDEV_PATH")
	if path == "" {
		t.Skip("MAPPLE_TEST_CHARDEV_PATH not set; skipping chardev integration test")
	}
	c, err := NewCharDev(context.Background(), path)
	if err != nil {
		t.Fatalf("NewCharDev(%q) error = %v", path, err)
	}
	defer c.Close()

	info, err := c.Info(context.Background())
	if err != nil {
		t.Fatalf("Info() error = %v", err)
	}
	if info.MaxSessions != 8 {
		t.Errorf("MaxSessions = %d, want 8", info.MaxSessions)
	}

	payload := []byte("chardev-integration-payload")
	got, err := c.Echo(context.Background(), payload)
	if err != nil {
		t.Fatalf("Echo() error = %v", err)
	}
	if string(got) != string(payload) {
		t.Fatalf("Echo() = %q, want %q", got, payload)
	}
}

func TestIntegration_Echo(t *testing.T) {
	host, port := testTCPTarget(t)
	c, err := NewTCP(context.Background(), host, port)
	if err != nil {
		t.Fatalf("NewTCP() error = %v", err)
	}
	defer c.Close()

	payload := []byte("integration-test-payload")
	got, err := c.Echo(context.Background(), payload)
	if err != nil {
		t.Fatalf("Echo() error = %v", err)
	}
	if string(got) != string(payload) {
		t.Fatalf("Echo() = %q, want %q", got, payload)
	}
}
