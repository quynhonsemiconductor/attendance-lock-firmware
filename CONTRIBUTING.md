# Contributing

- Trunk-based: short-lived branches, PR into `main`, squash merge.
- Conventional Commits; the PR title check enforces it.
- CI must pass: `build-dev`, `build-prod`, `host-tests`, plus the shared
  security gates (gitleaks/Semgrep, workflow security, PR title).
- Logic changes come with tests. Host-testable logic (parsers, ring and
  seq/cursor math) goes in `tests/host/`.
- Secrets never enter this repository: `secrets.h`-style files and any
  token/URL material are for runtime provisioning only. gitleaks runs on
  every push.
- Releases: release-please opens the release PR; a maintainer merges it only
  on an explicit "go release".
