# Claude Code Instructions

**This file is intentionally minimal.** All durable agent guidance — protocol authority, feature parity, platform primitives, improvement backlog — lives in [`AGENTS.md`](./AGENTS.md).

Before doing anything in this repository:

1. Read **[AGENTS.md](./AGENTS.md)** in full. It is the single source of truth.
2. Read **[SPECIFICATION.md](./SPECIFICATION.md)** before changing protocol, memory layout, or slot semantics.
3. If your change crosses the `xll-gen` or `types` repo boundary, read those repos' `AGENTS.md` too.
4. Do **not** add project-specific guidance to this file. Add it to `AGENTS.md` so every agent tool (Claude Code, Codex, Cursor, Aider, etc.) sees it.

Updating `CLAUDE.md` to anything other than this redirect is a policy violation; update `AGENTS.md` instead.
