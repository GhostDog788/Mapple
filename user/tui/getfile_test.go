package tui

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/GhostDog788/Mapple/user/api"
)

func TestGetfileCommandStreamsBinaryFile(t *testing.T) {
	data := strings.Repeat("binary\x00\xff\n", 20000)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body api.GetfileRequest
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Path != "/tmp/a  b/שלום" {
			t.Errorf("path: %+v %v", body, err)
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", fmt.Sprint(len(data)))
		for offset := 0; offset < len(data); {
			end := min(offset+4096, len(data))
			w.Write([]byte(data[offset:end]))
			offset = end
		}
	}))
	defer server.Close()
	client, _ := api.NewClientWithResponses(server.URL)
	m := New(client, "test", Profile{}, nil, "")
	m.current = &api.Session{Id: "test"}
	target := filepath.Join(t.TempDir(), "local file")
	updated, command := m.dispatch(fmt.Sprintf("command getfile %q %q", "/tmp/a  b/שלום", target))
	if command == nil {
		t.Fatal("no download dispatched")
	}
	m = updated.(Model)
	message := command()
	result := message.(getfileResultMsg)
	if result.err != nil || result.bytes != int64(len(data)) || result.path != target {
		t.Fatalf("download: %+v", result)
	}
	got, err := os.ReadFile(target)
	if err != nil || string(got) != data {
		t.Fatal("file contents differ", err)
	}
	updated, _ = m.Update(message)
	if updated.(Model).downloadCancel != nil || !strings.Contains(updated.(Model).notice, "saved") {
		t.Fatal("download state not cleared")
	}
	assertNoPartialFiles(t, filepath.Dir(target))
}

func assertNoPartialFiles(t *testing.T, dir string) {
	t.Helper()
	files, _ := filepath.Glob(filepath.Join(dir, ".mapple-getfile-*"))
	if len(files) != 0 {
		t.Fatalf("partial files leaked: %v", files)
	}
}

func TestGetfileOverwritesOnlyAfterSuccessfulDownload(t *testing.T) {
	for _, mode := range []string{"empty", "truncated", "error", "existing", "collision", "canceled", "bad content type"} {
		t.Run(mode, func(t *testing.T) {
			dir := t.TempDir()
			target := filepath.Join(dir, "output")
			if mode != "collision" {
				if err := os.WriteFile(target, []byte("original"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if mode == "collision" {
					os.WriteFile(target, []byte("original"), 0600)
				}
				if data, err := os.ReadFile(target); err != nil || string(data) != "original" {
					t.Error("destination changed before download completed")
				}
				if mode == "error" {
					w.Header().Set("Content-Type", "application/json")
					w.WriteHeader(422)
					json.NewEncoder(w).Encode(api.Error{Message: "permission denied"})
					return
				}
				w.Header().Set("Content-Type", "application/octet-stream")
				if mode == "bad content type" {
					w.Header().Set("Content-Type", "application/json")
				}
				if mode == "empty" {
					w.Header().Set("Content-Length", "0")
					return
				}
				w.Header().Set("Content-Length", "6")
				w.WriteHeader(200)
				if mode == "canceled" {
					w.(http.Flusher).Flush()
					cancel()
					<-r.Context().Done()
					return
				}
				if mode == "truncated" {
					w.Write([]byte("abc"))
					return
				}
				w.Write([]byte("abcdef"))
			}))
			defer server.Close()
			client, _ := api.NewClientWithResponses(server.URL)
			result := downloadFile(ctx, client, "test", "/file", target)
			data, err := os.ReadFile(target)
			if mode == "empty" || mode == "existing" || mode == "collision" {
				want := "abcdef"
				if mode == "empty" {
					want = ""
				}
				if result.err != nil || result.bytes != int64(len(want)) || err != nil || string(data) != want {
					t.Fatalf("replacement: result=%+v data=%q err=%v", result, data, err)
				}
			} else {
				if result.err == nil {
					t.Fatal("expected failure")
				}
				if err != nil || string(data) != "original" {
					t.Fatal("failed download changed the existing file")
				}
				if mode == "error" && !strings.Contains(result.err.Error(), "permission denied") {
					t.Fatal(result.err)
				}
			}
			assertNoPartialFiles(t, dir)
		})
	}
}

func TestGetfileArgumentParsingAndCancel(t *testing.T) {
	args, err := getfileArgs(`getfile "/a  b" "./out\"file"`)
	if err != nil || len(args) != 3 || args[1] != "/a  b" || args[2] != "./out\"file" {
		t.Fatalf("args=%q err=%v", args, err)
	}
	m := New(nil, "test", Profile{}, nil, "")
	m.current = &api.Session{Id: "test"}
	for _, line := range []string{"command getfile", "command getfile /a", "command getfile help", `command getfile "/unclosed`, `command getfile "/a"x /b`} {
		if _, cmd := m.dispatch(line); cmd != nil {
			t.Fatalf("unexpected command: %q", line)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	m.downloadCancel = cancel
	if _, cmd := m.dispatch("command getfile /another ./out"); cmd != nil {
		t.Fatal("overlapping download")
	}
	m.dispatch("command getfile cancel")
	if ctx.Err() == nil {
		t.Fatal("cancel not called")
	}
	updated, cmd := m.quit()
	if cmd != nil || !updated.(Model).downloadQuitting {
		t.Fatal("quit did not wait for download cleanup")
	}
	_, cmd = updated.(Model).Update(getfileResultMsg{err: context.Canceled})
	if cmd == nil {
		t.Fatal("quit not resumed")
	}
}
