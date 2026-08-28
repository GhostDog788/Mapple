package tui

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/GhostDog788/Mapple/user/api"
)

func NewClient(p Profile) (api.ClientWithResponsesInterface, error) {
	tlsConfig := &tls.Config{MinVersion: tls.VersionTLS12}
	if p.InsecureSkipVerify {
		tlsConfig.InsecureSkipVerify = true
	} else if p.CAFile != "" {
		pem, err := os.ReadFile(p.CAFile)
		if err != nil {
			return nil, fmt.Errorf("tui: read ca_file %s: %w", p.CAFile, err)
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(pem) {
			return nil, fmt.Errorf("tui: ca_file %s contains no usable certificates", p.CAFile)
		}
		tlsConfig.RootCAs = pool
	}

	httpClient := &http.Client{
		Timeout:   60 * time.Second,
		Transport: &http.Transport{TLSClientConfig: tlsConfig},
	}

	auth := func(ctx context.Context, req *http.Request) error {
		req.Header.Set("Authorization", "Bearer "+p.BearerToken)
		return nil
	}

	// api/openapi.yaml declares `servers: - url: /v1`; the generated client
	// takes the base URL literally rather than reading that, so it has to
	// be appended here.
	return api.NewClientWithResponses(strings.TrimRight(p.ServerURL, "/")+"/v1",
		api.WithHTTPClient(httpClient), api.WithRequestEditorFn(auth))
}
