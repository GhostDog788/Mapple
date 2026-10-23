package tui

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"unicode"

	tea "charm.land/bubbletea/v2"
	"github.com/GhostDog788/Mapple/user/api"
)

// Only GETFILE uses this parser. Quoted arguments use JSON string escapes;
// unquoted paths are literal, with no shell expansion or command execution.
func getfileArgs(input string) ([]string, error) {
	var args []string
	for input = strings.TrimSpace(input); input != ""; input = strings.TrimSpace(input) {
		end := strings.IndexFunc(input, unicode.IsSpace)
		if end < 0 {
			end = len(input)
		}
		value := ""
		if input[0] == '"' {
			end = 1
			for end < len(input) {
				if input[end] == '\\' {
					end += 2
					continue
				}
				if input[end] == '"' {
					end++
					break
				}
				end++
			}
			if end > len(input) {
				end = len(input)
			}
			if err := json.Unmarshal([]byte(input[:end]), &value); err != nil {
				return nil, fmt.Errorf("invalid quoted path: %w", err)
			}
			if end < len(input) && strings.IndexFunc(input[end:], unicode.IsSpace) != 0 {
				return nil, fmt.Errorf("separate paths with whitespace")
			}
		} else {
			value = input[:end]
		}
		args = append(args, value)
		input = input[end:]
	}
	return args, nil
}

func (m Model) fetchGetfile(ctx context.Context, id, remote, local string) tea.Cmd {
	return func() tea.Msg { return downloadFile(ctx, m.client, id, remote, local) }
}

func downloadFile(ctx context.Context, client Client, id, remote, local string) (result getfileResultMsg) {
	target, err := filepath.Abs(local)
	if err != nil {
		result.err = err
		return
	}
	// Same directory permits atomic publication after all bytes arrive.
	tmp, err := os.CreateTemp(filepath.Dir(target), ".mapple-getfile-*")
	if err != nil {
		result.err = err
		return
	}
	defer os.Remove(tmp.Name())
	defer tmp.Close()

	resp, err := client.PostSessionsIdCommandsGetfile(ctx, id, api.GetfileRequest{Path: remote})
	if err != nil {
		result.err = err
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		var apiErr api.Error
		if err := json.NewDecoder(io.LimitReader(resp.Body, 64*1024)).Decode(&apiErr); err == nil && apiErr.Message != "" {
			result.err = fmt.Errorf("%s", apiErr.Message)
		} else {
			result.err = fmt.Errorf("getfile: HTTP %d", resp.StatusCode)
		}
		return
	}
	mediaType, _, err := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/octet-stream" || resp.ContentLength < 0 {
		result.err = fmt.Errorf("invalid download response: expected file bytes with Content-Length")
		return
	}
	n, err := io.CopyBuffer(tmp, resp.Body, make([]byte, 32*1024))
	if err != nil {
		result.err = fmt.Errorf("download incomplete: %w", err)
		return
	}
	if n != resp.ContentLength {
		result.err = fmt.Errorf("download incomplete: received %d of %d bytes", n, resp.ContentLength)
		return
	}
	if err := ctx.Err(); err != nil {
		result.err = err
		return
	}
	if err := tmp.Sync(); err != nil {
		result.err = err
		return
	}
	if err := tmp.Close(); err != nil {
		result.err = err
		return
	}
	// Replace the destination only after the complete download is on disk.
	// A failure before this point leaves the previous destination intact.
	if err := os.Rename(tmp.Name(), target); err != nil {
		result.err = fmt.Errorf("publish download: %w", err)
		return
	}
	result.path, result.bytes = target, n
	return
}

// Wait for the canceled download to remove its temporary file before exiting.
func (m Model) quit() (tea.Model, tea.Cmd) {
	if m.downloadCancel != nil {
		m.downloadCancel()
		m.downloadQuitting = true
		return m, nil
	}
	return m, tea.Quit
}
