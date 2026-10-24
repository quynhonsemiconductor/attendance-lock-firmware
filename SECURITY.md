# Security Policy

## Supported

The `main` branch. The prototype under `prototype/` is explicitly OUT of
scope: it is a bench tool with no production hardening.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on this repository
(Security → Report a vulnerability). Do not open a public issue.

For the physical device: free egress is mechanical (inside handle), so
firmware flaws cannot trap people inside. Report firmware issues anyway —
the access-control and data-protection properties (ADR v2 §2.6–2.8) depend
on them.

## What the production profile promises

Secure Boot v2, release-mode flash encryption, secure ROM download mode,
JTAG revoked via Secure Boot (USB-Serial-JTAG burned off before first boot),
HMAC-based NVS encryption. Signing keys and the eFuse plan are offline-only
(docs/PROVISIONING.md). The terminal
token is stored so that reading it requires a flash read-out through those
mitigations. None of this applies to `dev`-profile builds.
