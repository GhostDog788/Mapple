package server

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"

	"github.com/GhostDog788/Mapple/user/api"
)

const healthLivePath = "/v1/health/live"

// Handler returns the complete daemon HTTP handler: audit logging wraps
// bearer auth (exempting only health/live) wraps the OpenAPI-generated
// routes, mounted under the /v1 base path from openapi.yaml's servers entry.
func (s *Server) Handler() http.Handler {
	strict := api.NewStrictHandler(s, nil)
	apiMux := http.NewServeMux()
	apiHandler := api.HandlerFromMux(strict, apiMux)

	top := http.NewServeMux()
	top.Handle("/v1/", http.StripPrefix("/v1", pslistFormatMiddleware(apiHandler)))

	// bearerAuth must run first: it's what injects identity into the
	// request context, and auditLog needs that to already be there when
	// it reads r.Context() on the request object it's actually handed.
	return bearerAuth(s.cfg, healthLivePath, auditLog(s.Logger, top))
}

// pslistFormatMiddleware implements the pslist request body's
// format: compact|pretty option. oapi-codegen's generated strict-server
// always encodes its response compactly (the stdlib json.Marshal default)
// with no per-request hook into that, so a "pretty" request is served by
// running the real handler through a ResponseRecorder and re-indenting the
// already-valid JSON bytes it produced -- correct regardless of how the
// body was produced, and isolated to this one route. "compact" (the
// default, and every other route) passes the response straight through.
func pslistFormatMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || !strings.HasSuffix(r.URL.Path, "/commands/pslist") {
			next.ServeHTTP(w, r)
			return
		}
		pretty := false
		if r.Body != nil {
			body, err := io.ReadAll(r.Body)
			r.Body.Close()
			if err == nil {
				var parsed struct {
					Format string `json:"format"`
				}
				_ = json.Unmarshal(body, &parsed)
				pretty = parsed.Format == "pretty"
			}
			r.Body = io.NopCloser(bytes.NewReader(body))
		}
		if !pretty {
			next.ServeHTTP(w, r)
			return
		}

		rec := httptest.NewRecorder()
		next.ServeHTTP(rec, r)

		header := w.Header()
		for k, v := range rec.Header() {
			header[k] = v
		}
		body := rec.Body.Bytes()
		var indented bytes.Buffer
		if json.Indent(&indented, body, "", "  ") == nil {
			body = indented.Bytes()
		}
		header.Set("Content-Length", strconv.Itoa(len(body)))
		w.WriteHeader(rec.Code)
		w.Write(body) //nolint:errcheck
	})
}
