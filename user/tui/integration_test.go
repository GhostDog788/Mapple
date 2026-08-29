//go:build integration

package tui

import (
	"os"
	"testing"

	"github.com/GhostDog788/Mapple/user/api"
)

// go test -tags=integration ./tui/... against a real running mappled, e.g.:
//
//	MAPPLE_TUI_SERVER_URL=https://127.0.0.1:8443 MAPPLE_TUI_TOKEN=... go test -tags=integration ./tui/...
//
// Drives the Model's actual Cmd/Update logic directly (same-package test,
// so the unexported *Cmd methods are reachable) rather than through a real
// terminal, since that's what would actually break if the wiring to the
// generated HTTP client were wrong.
func testProfile(t *testing.T) Profile {
	t.Helper()
	serverURL := os.Getenv("MAPPLE_TUI_SERVER_URL")
	if serverURL == "" {
		t.Skip("MAPPLE_TUI_SERVER_URL not set; skipping integration test")
	}
	token := os.Getenv("MAPPLE_TUI_TOKEN")
	if token == "" {
		t.Fatal("MAPPLE_TUI_TOKEN must be set alongside MAPPLE_TUI_SERVER_URL")
	}
	return Profile{ServerURL: serverURL, BearerToken: token, InsecureSkipVerify: true}
}

func mustModel(t *testing.T) Model {
	t.Helper()
	client, err := NewClient(testProfile(t))
	if err != nil {
		t.Fatalf("NewClient() error = %v", err)
	}
	return New(client, "dev", testProfile(t), nil, "")
}

func TestIntegration_FullFlow(t *testing.T) {
	host := os.Getenv("MAPPLE_TUI_MODULE_HOST")
	if host == "" {
		host = "192.168.122.77"
	}
	m := mustModel(t)

	// 1. Load sessions (should succeed even with zero leases).
	msg := m.loadSessions()()
	loaded, ok := msg.(sessionsLoadedMsg)
	if !ok || loaded.err != nil {
		t.Fatalf("loadSessions: %+v", msg)
	}

	// 2. Create a session against the real kernel module.
	port := 4444
	dest := api.Destination{Type: api.Tcp, Host: &host, Port: &port}
	msg = m.createSession(dest)()
	created, ok := msg.(sessionCreatedMsg)
	if !ok || created.err != nil {
		t.Fatalf("createSession: %+v", msg)
	}
	updated, _ := m.Update(created)
	m = updated.(Model)
	if !m.created[created.session.Id] {
		t.Fatal("created session id not tracked for auto-renew")
	}
	id := created.session.Id
	defer func() {
		m.deleteSession(id)()
	}()

	// 3. INFO.
	msg = m.fetchInfo(id)()
	info, ok := msg.(infoResultMsg)
	if !ok || info.err != nil {
		t.Fatalf("fetchInfo: %+v", msg)
	}
	if info.info.MaxSessions != 8 {
		t.Errorf("MaxSessions = %d, want 8", info.info.MaxSessions)
	}

	// 4. ECHO.
	msg = m.sendEcho(id, "tui-integration-test")()
	echo, ok := msg.(echoResultMsg)
	if !ok || echo.err != nil {
		t.Fatalf("sendEcho: %+v", msg)
	}
	if string(echo.data) != "tui-integration-test" {
		t.Errorf("echo data = %q", echo.data)
	}

	// 5. PSLIST, and the Model-level task extraction that feeds the list view.
	msg = m.fetchPslist(id, nil)()
	pslist, ok := msg.(pslistResultMsg)
	if !ok || pslist.err != nil {
		t.Fatalf("fetchPslist: %+v", msg)
	}
	tasks := tasksFromSnapshot(pslist.snapshot)
	if len(tasks) == 0 {
		t.Fatal("tasksFromSnapshot returned no tasks")
	}
	var foundPID1 bool
	for _, task := range tasks {
		if task.pid == 1 {
			foundPID1 = true
			if task.comm != "systemd" {
				t.Errorf("pid 1 comm = %q, want systemd", task.comm)
			}
			if !task.hasIdentity || task.exePath != "/usr/lib/systemd/systemd" {
				t.Errorf("pid 1 identity/exePath = %v/%q", task.hasIdentity, task.exePath)
			}
		}
	}
	if !foundPID1 {
		t.Fatal("no pid 1 task in tasksFromSnapshot output")
	}

	// 6. Renew, then delete, then confirm it's gone from the list.
	msg = m.renewSession(id)()
	renewed, ok := msg.(sessionRenewedMsg)
	if !ok || renewed.err != nil {
		t.Fatalf("renewSession: %+v", msg)
	}

	msg = m.deleteSession(id)()
	deleted, ok := msg.(sessionDeletedMsg)
	if !ok || deleted.err != nil {
		t.Fatalf("deleteSession: %+v", msg)
	}

	msg = m.loadSessions()()
	loaded, ok = msg.(sessionsLoadedMsg)
	if !ok || loaded.err != nil {
		t.Fatalf("loadSessions after delete: %+v", msg)
	}
	for _, s := range loaded.sessions {
		if s.Id == id {
			t.Fatal("deleted session still present in list")
		}
	}
}
