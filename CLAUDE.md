# vectorworks-mcp (Gantry)

MCP server + Vectorworks plugin that lets an agent run Python inside a live Vectorworks 2026
on Windows. Read `docs/agent-guide.md` first: tool contract, `run_script` semantics, caveats,
recovery. `docs/capabilities-matrix.md` is generated evidence (do not edit by hand).

## Ground rules

- Never test against a document that is not disposable. `scripts/vw_relaunch.ps1 -Force`
  relaunches VW on a scratch copy of the blank test file (`%TEMP%\gantry-test.vwx`).
- `run_script`: one undo step per call, stderr output means failure and rollback, globals do
  not persist between calls, 25 s ceiling, never open dialogs, never pass `py_mode=execute`.
- After `script_crashed` / `engine_poisoned`, Vectorworks must be restarted.
- Keep LF line endings. Commit after each green step with a message saying what was verified.

## Verify before claiming

- `python scripts/capability_bench.py` (full suite + latency; `--slow`, `--ifc PATH`)
- `python scripts/gantry_ladder.py` (write-safety ladder over raw TCP)
- `python scripts/mcp_smoke.py` (stdio server, read-only tools)
- `python scripts/vw_run.py file.py` (run one Python file inside VW and print the result)
- Spotlight-specific recipes and caveats: `docs/spotlight-recipes.md`
- `cargo test` in `mcp-server/`

## Build and deploy

Rust: `cargo build --release` in `mcp-server/`. Plugin: MSBuild command and SDK paths in
`FORK_NOTES.md` ("What was actually built and how to rebuild"); deploy copies
`vw-plugin/build/Release-VW2026/vectorworks-mcp.vlb` and `vectorworks-mcp.vwr/` into
`%APPDATA%\Nemetschek\Vectorworks\2026\Plug-ins\`, then VW must restart.

## Docs discipline

Decisions go in `docs/decisions/NNN-title.md`. Session history and deployment facts go in
`FORK_NOTES.md` (newest section first). `docs/mcp-landscape-2026-10.md` is a survey; leave it
unless contradicted.
