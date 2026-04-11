// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"encoding/json"
	"fmt"
)

// ProtocolVersion is the wire-format schema version we currently
// speak. Bumped when the envelope or any message shape changes
// incompatibly. See docs/wire-protocol.md for the policy.
const ProtocolVersion = 1

// MessageType identifies the kind of an envelope's payload.
type MessageType string

const (
	TypeHello       MessageType = "hello"
	TypeHelloOK     MessageType = "hello_ok"
	TypeRegister    MessageType = "register"
	TypeRegisterOK  MessageType = "register_ok"
	TypeDeregister  MessageType = "deregister"
	TypeReload      MessageType = "reload"
	TypeReloadAck   MessageType = "reload_ack"
	TypeShutdown    MessageType = "shutdown"
	TypeError       MessageType = "error"
)

// Envelope is the shared wrapper every message on the wire is
// marshaled inside. The discriminated payload lives in fields that
// are type-specific; unmarshaling is two-pass (peek at t, then
// unmarshal the concrete shape).
//
// Fields unused by a given type are zero/empty and are allowed to
// be omitted by the sender — unknown fields on receive are ignored
// for forward compatibility.
type Envelope struct {
	Type MessageType `json:"t"`
	V    int         `json:"v"`
	Seq  uint64      `json:"seq"`

	// hello
	WrapperVersion string `json:"wrapper_version,omitempty"`
	Pid            int    `json:"pid,omitempty"`

	// hello_ok, register_ok, reload_ack (and error)
	AckSeq uint64 `json:"ack_seq,omitempty"`

	// hello_ok
	DaemonVersion string `json:"daemon_version,omitempty"`

	// register
	Name        string `json:"name,omitempty"`
	ChildPid    int    `json:"child_pid,omitempty"`
	ChildBinary string `json:"child_binary,omitempty"`

	// register_ok
	ConfigFound bool `json:"config_found,omitempty"`
	Polling     bool `json:"polling,omitempty"`

	// reload
	OldVersion string `json:"old_version,omitempty"`
	NewVersion string `json:"new_version,omitempty"`
	Reason     string `json:"reason,omitempty"`

	// reload_ack
	Status string `json:"status,omitempty"`
	Detail string `json:"detail,omitempty"`

	// error
	Code string `json:"code,omitempty"`
}

// Marshal returns the JSON bytes of an envelope, not terminated with
// a newline — the caller (framing layer) appends the newline.
func (e *Envelope) Marshal() ([]byte, error) {
	if e.V == 0 {
		e.V = ProtocolVersion
	}
	return json.Marshal(e)
}

// Unmarshal decodes bytes (without a trailing newline) into an
// Envelope. Unknown fields are permitted per the wire-protocol spec.
func Unmarshal(data []byte) (*Envelope, error) {
	var e Envelope
	if err := json.Unmarshal(data, &e); err != nil {
		return nil, fmt.Errorf("decode envelope: %w", err)
	}
	if e.V != ProtocolVersion {
		return nil, fmt.Errorf("unsupported protocol version v=%d (this daemon speaks v=%d)", e.V, ProtocolVersion)
	}
	if e.Type == "" {
		return nil, fmt.Errorf("envelope missing type")
	}
	return &e, nil
}
