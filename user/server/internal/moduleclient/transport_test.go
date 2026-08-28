package moduleclient

import (
	"os"
	"path/filepath"
	"testing"
)

func TestOpenCharDev_RejectsRelativePath(t *testing.T) {
	if _, err := openCharDev("relative/path"); err == nil {
		t.Fatal("openCharDev() with relative path: want error, got nil")
	}
}

func TestOpenCharDev_RejectsNonCharDevice(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "regular-file")
	if err := os.WriteFile(path, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := openCharDev(path); err == nil {
		t.Fatal("openCharDev() on regular file: want error, got nil")
	}
}

func TestOpenCharDev_AcceptsRealCharDevice(t *testing.T) {
	tr, err := openCharDev("/dev/null")
	if err != nil {
		t.Fatalf("openCharDev(/dev/null) error = %v", err)
	}
	defer tr.Close()
}
