#!/usr/bin/env bash
# Debian uses the same dpkg-buildflags contract as Ubuntu.
exec "$(dirname "$0")/ubuntu.sh"
