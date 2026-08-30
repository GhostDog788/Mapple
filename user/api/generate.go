// Package api holds the OpenAPI contract and its generated Go types/server
// interface. Regenerate with `go generate ./...` from user/, or `make
// api-verify` from the repo root to check nothing drifted.
package api

//go:generate go tool oapi-codegen -config codegen.yaml openapi.yaml
