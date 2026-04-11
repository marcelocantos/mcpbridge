# typed: false
# frozen_string_literal: true

# Homebrew formula for mcpbridge. Copy this file into a tap repo
# (typically marcelocantos/homebrew-tap/Formula/mcpbridge.rb) before
# publishing a release. The release workflow in the main repo is
# responsible for bumping `url` and `sha256` on each tag.

class Mcpbridge < Formula
  desc "Keep MCP server sessions alive across upgrades"
  homepage "https://github.com/marcelocantos/mcpbridge"
  url "https://github.com/marcelocantos/mcpbridge/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "REPLACE_ME_ON_RELEASE"
  license "Apache-2.0"
  head "https://github.com/marcelocantos/mcpbridge.git", branch: "master"

  depends_on "go" => :build

  def install
    # mcpbridge's top-level Makefile builds both the C wrapper and
    # the Go daemon. The wrapper has no external build deps beyond
    # a C11 compiler (which every macOS install ships via Xcode CLI
    # tools); the daemon needs Go, declared above.
    system "make", "VERSION=#{version}"

    bin.install "wrapper/mcpbridge"
    bin.install "daemon/mcpbridge-daemon"

    # Lay down the config directories so brew-managed installs have
    # somewhere predictable to drop per-server config files.
    (etc/"mcpbridge").mkpath
    (share/"mcpbridge").mkpath
  end

  # brew services start mcpbridge will launch this definition. The
  # daemon is resilient to being killed — brew services will
  # restart it automatically, which is exactly what we want for a
  # long-lived upgrade poller.
  service do
    run [opt_bin/"mcpbridge-daemon"]
    run_type :immediate
    keep_alive true
    working_dir var
    log_path var/"log/mcpbridge-daemon.log"
    error_log_path var/"log/mcpbridge-daemon.log"
  end

  def caveats
    <<~EOS
      To start the daemon as a background service:
        brew services start mcpbridge

      Per-server config files live at:
        ~/.config/mcpbridge/*.json         (user, takes precedence)
        #{share}/mcpbridge/*.json          (system, for brew-managed)

      In your MCP client config, prefix each MCP server command
      with mcpbridge:
        {
          "command": "#{opt_bin}/mcpbridge",
          "args": ["--", "real-mcp-server", "--any", "--flags"]
        }

      See #{homepage}/blob/master/docs/packaging.md for details.
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/mcpbridge --version")
    assert_match version.to_s, shell_output("#{bin}/mcpbridge-daemon --version")
  end
end
