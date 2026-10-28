# door terminal contract v1 fixtures

Derived from the opshub repository
(docs/contracts/door-terminal-v1/) — the wire contract the sync component
encodes and decodes (ADR §2.4). The host tests compare real
encode/decode output against these files.

- Contract: door terminal contract v1
- Copied: 2026-10-24
- Windows re-pinned: 2026-10-28 — `valid_from`/`valid_until` are INTEGER
  epoch seconds (UTC) or null (contract decision 2026-10-28; the ISO-8601
  draft would have blown the 24 KiB response buffer at 256 cards). The
  allow-list fixture now carries all three shapes: permanent card (both
  null), fully windowed card, and open-ended card (from set, until null).

The upstream contract is still evolving; re-sync these files whenever it
changes.
