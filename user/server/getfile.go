package server

import (
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"strconv"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient"
	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

// The generated buffered client is unsuitable for downloads. This response
// streams the module body and aborts HTTP on failure after headers are sent.
type getfileResponse struct {
	file     *moduleclient.FileDownload
	sessions *SessionManager
	leaseID  string
	logger   *slog.Logger
}

func (r *getfileResponse) VisitPostSessionsIdCommandsGetfileResponse(w http.ResponseWriter) error {
	defer func() {
		r.sessions.Renew(r.leaseID)
		r.file.Close()
	}()
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(r.file.Size, 10))
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	if err := http.NewResponseController(w).Flush(); err != nil {
		panic(http.ErrAbortHandler)
	}
	n, err := io.CopyBuffer(w, r.file, make([]byte, 32*1024))
	if err != nil || n != r.file.Size {
		r.logger.Warn("file download interrupted", "lease_id", r.leaseID, "bytes", n, "expected", r.file.Size, "error", err)
		// Returning an error here would let the generated handler append an
		// HTTP error message to file contents. Abort the stream instead.
		panic(http.ErrAbortHandler)
	}
	return nil
}

func getfileStatusMessage(status wire.Status) string {
	switch status {
	case wire.StatusNotFound:
		return "file or parent directory not found on the module's machine"
	case wire.StatusPermissionDenied:
		return "permission denied opening the file on the module's machine"
	case wire.StatusBusy:
		return "module is busy"
	case wire.StatusUnsupportedCommand:
		return "module does not support GETFILE"
	case wire.StatusResourceExhausted:
		return "module has insufficient resources for the download"
	default:
		return fmt.Sprintf("module could not open the file (status %d)", status)
	}
}
