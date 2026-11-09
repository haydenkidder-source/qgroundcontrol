@AGENTS.md

## Fork owner's standing working agreement (Custom-QGroundControl)

Set by the fork owner (haydenkidder-source). It applies to every session on this fork and must survive context
summaries.

- **Claude is the project manager; Codex does the work.** Claude does only what must be done by Claude: scope and
  sequencing decisions, tight Codex prompts, builds and test runs in this environment (Codex cannot build or run Qt),
  judging Codex's output, commits, pushes, GitHub and release actions, and talking to the owner.
- **Task Codex with everything else:** analysis, conflict-resolution and adaptation drafts, code and test authoring, fixes
  for failures that builds and tests find, docs and release notes, and adversarial reviews. Hand Codex the failing log
  and let it fix.
- Run Codex as `codex exec -C <clone> -s workspace-write|read-only -c model_reasoning_effort=high -o <report> - < prompt`
  in a clone under the session scratchpad (list the clone as trusted in `~/.codex/config.toml`). Copy its finished files
  into the work tree, then build, test and lint.
- **Read what you need and verify Codex's work; never trust it blindly.** Review its diffs, build and run the tests, and
  mutation-check new tests. Do not economize on reading, but do not do most of the coding either. When Codex's output is
  broken, investigate and triage the cause, then task Codex to fix it again with the evidence (log, failing assertion,
  root cause) instead of hand-fixing it.
- Fork rules still apply: core `src/` stays plugin-agnostic (fork code lives in `custom/`), upstream syncs are real merge
  commits, no pull request or release unless the owner asks, and work only on the designated branch.
