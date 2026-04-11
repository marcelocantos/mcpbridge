// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

package socket

import (
	"testing"
)

func TestEnvelopeRoundTrip(t *testing.T) {
	cases := []Envelope{
		{
			Type: TypeHello, V: 1, Seq: 1,
			WrapperVersion: "0.1.0", Pid: 12345,
		},
		{
			Type: TypeRegister, V: 1, Seq: 2,
			Name: "mnemo", ChildPid: 12346,
			ChildBinary: "/opt/homebrew/bin/mnemo",
		},
		{
			Type: TypeRegisterOK, V: 1, Seq: 3, AckSeq: 2,
			ConfigFound: false, Polling: false,
		},
		{
			Type: TypeReload, V: 1, Seq: 7,
			Name: "mnemo", OldVersion: "0.4.2", NewVersion: "0.5.0",
			Reason: "brew_upgrade",
		},
		{
			Type: TypeReloadAck, V: 1, Seq: 4, AckSeq: 7,
			Status: "ok",
		},
		{
			Type: TypeError, V: 1, Seq: 5,
			Code: "unsupported_version", Detail: "v=2",
		},
	}
	for _, want := range cases {
		t.Run(string(want.Type), func(t *testing.T) {
			data, err := want.Marshal()
			if err != nil {
				t.Fatalf("marshal: %v", err)
			}
			got, err := Unmarshal(data)
			if err != nil {
				t.Fatalf("unmarshal: %v", err)
			}
			if got.Type != want.Type || got.Seq != want.Seq {
				t.Errorf("type/seq mismatch: got %+v, want %+v", got, want)
			}
		})
	}
}

func TestUnmarshalRejectsBadVersion(t *testing.T) {
	_, err := Unmarshal([]byte(`{"t":"hello","v":99,"seq":1}`))
	if err == nil {
		t.Fatal("expected error on unknown v")
	}
}

func TestUnmarshalRejectsMissingType(t *testing.T) {
	_, err := Unmarshal([]byte(`{"v":1,"seq":1}`))
	if err == nil {
		t.Fatal("expected error on missing t")
	}
}

func TestMarshalDefaultsVersion(t *testing.T) {
	e := &Envelope{Type: TypeHello, Seq: 1}
	data, err := e.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	got, err := Unmarshal(data)
	if err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got.V != ProtocolVersion {
		t.Errorf("default v not applied, got %d", got.V)
	}
}
