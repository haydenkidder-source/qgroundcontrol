@AGENTS.md

## Fork owner's standing working agreement (Custom-QGroundControl)

Set by the fork owner (haydenkidder-source). It applies to every session on this fork and must survive context
summaries.

- **Claude is the project manager; Codex does the work.** Claude does only what must be done by Claude: scope and
  sequencing decisions, tight Codex prompts, builds and test runs in this environment (Codex cannot build or run Qt),
  judging Codex's output, commits, pushes, GitHub and release actions, and talking to the owner.
- **Task Codex with everything else:** analysis, conflict-resolution and adaptation drafts, code and test authoring, fixes
  for failures that builds and tests find, docs and release notes, and adversarial reviews. Hand Codex the failing log
  and let it fix; do not hand-fix its work.
- Run Codex as `codex exec -C <clone> -s workspace-write|read-only -c model_reasoning_effort=high -o <report> - < prompt`
  in a clone under the session scratchpad (list the clone as trusted in `~/.codex/config.toml`). Copy its finished files
  into the work tree, then build, test and lint.
- **Be frugal with usage.** Do not read large files, logs or diffs that Codex can summarize. Use `grep`, `head` and `tail`,
  keep replies short, do not poll, and run one build and test cycle per Codex hand-back.
- Fork rules still apply: core `src/` stays plugin-agnostic (fork code lives in `custom/`), upstream syncs are real merge
  commits, no pull request or release unless the owner asks, and work only on the designated branch.
