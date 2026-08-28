package wire

import (
	"bytes"
	"errors"
	"testing"
)

// Bytes captured from a live, running mapple.ko (kernel v3 wire protocol)
// over TCP, not hand-typed, so these tests prove the Go encoder/decoder
// matches the real kernel rather than just this package's own assumptions.

var infoRequestBytes = []byte{
	0x4d, 0x41, 0x50, 0x4c, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

var infoResponseHeaderBytes = []byte{
	0x4d, 0x41, 0x50, 0x4c, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
}

var infoResponseBodyBytes = []byte{
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x08,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e,
}

var echoRequestHeaderBytes = []byte{
	0x4d, 0x41, 0x50, 0x4c, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14,
}

const echoPayloadLen = 20 // "golden-fixture-probe"

func TestHeaderEncode_InfoRequest(t *testing.T) {
	got := Header{Command: CommandInfo}.Encode()
	if !bytes.Equal(got[:], infoRequestBytes) {
		t.Fatalf("Encode() = % x, want % x", got, infoRequestBytes)
	}
}

func TestHeaderEncode_EchoRequest(t *testing.T) {
	got := Header{Command: CommandEcho, PayloadSize: echoPayloadLen}.Encode()
	if !bytes.Equal(got[:], echoRequestHeaderBytes) {
		t.Fatalf("Encode() = % x, want % x", got, echoRequestHeaderBytes)
	}
}

func TestDecodeHeader_InfoResponse(t *testing.T) {
	h, err := DecodeHeader(infoResponseHeaderBytes)
	if err != nil {
		t.Fatalf("DecodeHeader() error = %v", err)
	}
	want := Header{Command: CommandInfo, Flags: 0, Status: StatusOK, PayloadSize: InfoResponseSize}
	if h != want {
		t.Fatalf("DecodeHeader() = %+v, want %+v", h, want)
	}
}

func TestDecodeInfoResponse(t *testing.T) {
	info, err := DecodeInfoResponse(infoResponseBodyBytes)
	if err != nil {
		t.Fatalf("DecodeInfoResponse() error = %v", err)
	}
	want := InfoResponse{SessionID: 5, MaxSessions: 8, Reserved: 0, SupportedCommands: 0b1110}
	if info != want {
		t.Fatalf("DecodeInfoResponse() = %+v, want %+v", info, want)
	}
}

func TestDecodeHeader_WrongLength(t *testing.T) {
	if _, err := DecodeHeader(infoResponseHeaderBytes[:20]); err == nil {
		t.Fatal("DecodeHeader() with 20 bytes: want error, got nil")
	}
}

func TestDecodeHeader_BadVersion(t *testing.T) {
	buf := append([]byte(nil), infoResponseHeaderBytes...)
	buf[5] = 2 // version low byte: 3 -> 2
	if _, err := DecodeHeader(buf); !errors.Is(err, ErrBadMagicOrVersion) {
		t.Fatalf("DecodeHeader() error = %v, want ErrBadMagicOrVersion", err)
	}
}
