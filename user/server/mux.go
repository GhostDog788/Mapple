package server

import (
	"net/http"

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
	top.Handle("/v1/", http.StripPrefix("/v1", apiHandler))

	// bearerAuth must run first: it's what injects identity into the
	// request context, and auditLog needs that to already be there when
	// it reads r.Context() on the request object it's actually handed.
	return bearerAuth(s.cfg, healthLivePath, auditLog(s.Logger, top))
}
