# M6 raw-results archive

`m6-results-2026-10-13.tar.gz` (2,335,151 bytes) preserves the raw measurements behind
`docs/experiments/m6-cost-accounting.md`. Raw results are otherwise untracked (`results/` is git-ignored); this file exists
so they are not lost with the machine they were measured on. It is a record, not a build input.

SHA-256 of the archive: `14cc6ede42bb544e5b4caf09029147be35ba72cf53156fe6b15c29e86eaec357`

## Contents (top-level directory `m6-results-2026-10-13/`)

| path | what |
|---|---|
| `raw/` | the 12 result JSON files the M6 tables are computed from (blocks A, B, C, E) |
| `evidence/` | configure and build logs, run logs and exit codes, the phase-profile logs and timing files, the scorer-build probe, the smoke tests, the anomaly rule, and the full analysis output |
| `binaries/` | the four binaries that produced the results (`lpq_benchmark`, `lpq_profile`, `lpq_profile_noinstr`, and `lpq_benchmark.release-noinstr`) |
| `models/` | the two protocol-C static models and their reports, used as inputs |
| `PROVENANCE_FROM_COMMIT_e447d3cc.txt` | a copy of `docs/experiments/m6/provenance.txt`: the SHA-256 of every raw file, binary and input |
| `README.txt` | the archive's own note |

Measured source commit: `450804238ca8786fb45bdce9f70991fc1765d55f` (clean worktree, fresh builds). Not included: the two
processed graphs (regenerable; their hashes and header checksums are in the provenance file).

## Verify

From this directory:

    sha256sum -c m6-results-2026-10-13.tar.gz.sha256       # prints: m6-results-2026-10-13.tar.gz: OK
    tar -xzf m6-results-2026-10-13.tar.gz
    cd m6-results-2026-10-13/raw && sha256sum *.json        # compare with the "raw result files" section of
                                                            # PROVENANCE_FROM_COMMIT_e447d3cc.txt and of
                                                            # docs/experiments/m6/provenance.txt

The same hashes are in the repository at `docs/experiments/m6/provenance.txt`, so the archive can be checked against the
tracked record. The timings are host-specific measurements from one shared virtual machine; re-running will not reproduce them
exactly (see the M6 document).
