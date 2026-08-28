package moduleclient

import (
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

type transport interface {
	io.Reader
	io.Writer
	io.Closer
	SetDeadline(time.Time) error
}

func dialTCP(host string, port uint16) (transport, error) {
	conn, err := net.Dial("tcp", net.JoinHostPort(host, strconv.Itoa(int(port))))
	if err != nil {
		return nil, fmt.Errorf("moduleclient: dial %s:%d: %w", host, port, err)
	}
	return conn, nil
}

func openCharDev(path string) (transport, error) {
	if !filepath.IsAbs(path) {
		return nil, fmt.Errorf("moduleclient: chardev path must be absolute: %q", path)
	}
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	fi, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	if fi.Mode()&os.ModeCharDevice == 0 {
		f.Close()
		return nil, fmt.Errorf("moduleclient: %q is not a character device", path)
	}
	return f, nil
}
