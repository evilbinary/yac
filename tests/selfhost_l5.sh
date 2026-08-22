#!/usr/bin/env bash
# L5 entry point: yac → yc_A → yc_B and shared e2e.
exec sh tests/selfhost_bootstrap.sh "$@"
