//go:build integration

package moduleclient

import (
	"context"
	"os"
	"strconv"
	"testing"
)

// go test -tags=integration ./... against a real running mapple.ko, e.g.:
//
//	MAPPLE_TEST_TCP_HOST=192.168.122.77 MAPPLE_TEST_TCP_PORT=4444 go test -tags=integration ./...
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

func TestIntegration_ProcessSnapshot(t *testing.T) {
	host, port := testTCPTarget(t)
	c, err := NewTCP(context.Background(), host, port)
	if err != nil {
		t.Fatalf("NewTCP() error = %v", err)
	}
	defer c.Close()

	snap, err := c.ProcessSnapshot(context.Background(), DefaultGroups)
	if err != nil {
		t.Fatalf("ProcessSnapshot() error = %v", err)
	}
	if snap.TaskCount == 0 || int(snap.TaskCount) != len(snap.Tasks) {
		t.Fatalf("TaskCount=%d but len(Tasks)=%d", snap.TaskCount, len(snap.Tasks))
	}

	var pid1 *ProcessTask
	for _, task := range snap.Tasks {
		if task.PID == 1 {
			pid1 = task
			break
		}
	}
	if pid1 == nil {
		t.Fatal("no task with PID 1 in snapshot")
	}
	if pid1.Comm != "systemd" {
		t.Errorf("pid 1 Comm = %q, want systemd", pid1.Comm)
	}
	if pid1.IdentityID == nil {
		t.Fatal("pid 1 has no IdentityID link")
	}
	identity, ok := snap.Identities[*pid1.IdentityID]
	if !ok {
		t.Fatalf("no identity object for id %d", *pid1.IdentityID)
	}
	if identity.ExecutablePath != "/usr/lib/systemd/systemd" {
		t.Errorf("pid 1 ExecutablePath = %q", identity.ExecutablePath)
	}
	if pid1.SecurityID == nil {
		t.Fatal("pid 1 has no SecurityID link")
	}
	security, ok := snap.Securities[*pid1.SecurityID]
	if !ok || security.UID != 0 {
		t.Errorf("pid 1 security = %+v", security)
	}
	if pid1.FilesID == nil {
		t.Fatal("pid 1 has no FilesID link")
	}
	files, ok := snap.FilesObjects[*pid1.FilesID]
	if !ok || len(files.FDs) == 0 {
		t.Fatal("pid 1 has no fd entries")
	}
	foundKmsg := false
	for _, fd := range files.FDs {
		if fd.Path == "/dev/kmsg" {
			foundKmsg = true
			if fd.Details == nil {
				t.Error("/dev/kmsg fd has no Details (fds_details group requested)")
			}
		}
	}
	if !foundKmsg {
		t.Error("pid 1 has no /dev/kmsg fd")
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
