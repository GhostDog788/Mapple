package server

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"net/http"
	"strings"
)

type identityContextKey struct{}

// IdentityFromContext returns the caller's identity name, set by the bearer
// auth middleware on every authenticated request.
func IdentityFromContext(ctx context.Context) (string, bool) {
	name, ok := ctx.Value(identityContextKey{}).(string)
	return name, ok
}

func writeAuthError(w http.ResponseWriter, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusUnauthorized)
	w.Write([]byte(`{"code":"` + code + `","message":"` + message + `"}`))
}

// bearerAuth authenticates every request except path (health/live) against
// the configured identities, using a constant-time digest comparison, and
// stashes the matched identity name in the request context.
func bearerAuth(cfg *Config, exempt string, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == exempt {
			next.ServeHTTP(w, r)
			return
		}
		authorization := r.Header.Get("Authorization")
		token, ok := strings.CutPrefix(authorization, "Bearer ")
		if !ok || token == "" {
			writeAuthError(w, "unauthorized", "missing bearer token")
			return
		}
		digest := sha256.Sum256([]byte(token))
		for _, identity := range cfg.Identities {
			if subtle.ConstantTimeCompare(digest[:], identity.TokenSHA256[:]) == 1 {
				ctx := context.WithValue(r.Context(), identityContextKey{}, identity.Name)
				next.ServeHTTP(w, r.WithContext(ctx))
				return
			}
		}
		writeAuthError(w, "unauthorized", "invalid bearer token")
	})
}
