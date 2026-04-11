// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package scheduler owns the polling loop and fswatch integration:
// when to check each configured source, when to trigger an upgrade,
// and when to notify connected wrappers.
package scheduler
