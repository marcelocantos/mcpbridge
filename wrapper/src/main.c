/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* mcpbridge — generic C wrapper for MCP servers with auto-upgrade.
 *
 * This file is currently a skeleton. CLI parsing, transport wiring,
 * dispatch, state machine, and upgrade detection will land in later
 * sub-targets (see docs/targets.yaml). For now it only handles
 * --version / --help / --help-agent so the build and test harness
 * have something real to exercise. */

#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static const char usage_text[] =
    "Usage: mcpbridge [OPTIONS] -- COMMAND [ARGS...]\n"
    "       mcpbridge --http URL [OPTIONS]\n"
    "\n"
    "Wrap an MCP server and keep its session alive across upgrades.\n"
    "\n"
    "Options:\n"
    "  --config NAME          explicit config name\n"
    "  --check-interval DUR   override upgrade-check interval (e.g. 30m)\n"
    "  --upgrade MODE         override upgrade mode (off|notify|auto)\n"
    "  --log-transitions      log state machine transitions to stderr\n"
    "  -v, --verbose          extra logging\n"
    "  --version              print version and exit\n"
    "  --help                 print this help and exit\n"
    "  --help-agent           print agent-oriented help and exit\n";

static const char agent_help_text[] =
    "mcpbridge " MCPBRIDGE_VERSION "\n"
    "\n"
    "Wraps any MCP server (stdio or localhost HTTP) transparently to the\n"
    "upstream agent, detects when a new version is available, drains\n"
    "in-flight requests, runs the upgrade, and cycles the child — all\n"
    "without breaking the agent's MCP session.\n"
    "\n"
    "Typical usage (in your MCP client config):\n"
    "  { \"command\": \"mcpbridge\",\n"
    "    \"args\": [\"--\", \"real-mcp-server\", \"--some-flag\"] }\n"
    "\n"
    "Per-server upgrade metadata lives in JSON config files discovered\n"
    "from ~/.config/mcpbridge/<name>.json and $prefix/share/mcpbridge/\n"
    "<name>.json. See docs/config-schema.md for the schema.\n"
    "\n"
    "This build is a work in progress. See docs/targets.yaml for the\n"
    "implementation roadmap.\n";

int main(int argc, char **argv) {
    if (argc <= 1) {
        fputs(usage_text, stderr);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--version")) {
            puts(MCPBRIDGE_VERSION);
            return 0;
        }
        if (str_eq(argv[i], "--help") || str_eq(argv[i], "-h")) {
            fputs(usage_text, stdout);
            return 0;
        }
        if (str_eq(argv[i], "--help-agent")) {
            fputs(agent_help_text, stdout);
            return 0;
        }
        if (str_eq(argv[i], "--")) {
            /* End of mcpbridge options. The wrapped command begins at
             * argv[i+1]. Not yet implemented. */
            log_error("wrapping not yet implemented (see docs/targets.yaml)");
            return 69; /* EX_UNAVAILABLE */
        }
        if (str_has_prefix(argv[i], "-")) {
            fprintf(stderr, "mcpbridge: unknown option: %s\n", argv[i]);
            fputs(usage_text, stderr);
            return 2;
        }
        fprintf(stderr, "mcpbridge: unexpected argument: %s\n", argv[i]);
        fputs(usage_text, stderr);
        return 2;
    }

    fputs(usage_text, stderr);
    return 2;
}
