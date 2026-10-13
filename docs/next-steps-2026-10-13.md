# Next steps (written 2026-10-13, for pickup 2026-10-14)

Handoff for the next session. Written at the end of the session that produced commits
`5be0771`, `a965d4d`, `bab5518` on branch `claude/lattice-platform-mcp-TM9xi`. Read
`docs/agent-guide.md` first, then `docs/spotlight-recipes.md`. This file says where things
stand, what was decided, what is still open, and exactly what to do next.

## 1. Where things stand

- Server, plugin and docs are committed and pushed through `bab5518`. Working tree clean.
- Verified on Vectorworks Spotlight 2026, exe 31.7.0.879357, plugin SDK 3100, Python 3.9.2
  inside VW. `scripts/capability_bench.py` scores 34/34 (record
  `docs/test-runs/bench-2026-10-12-203100.json`).
- Spotlight recipes verified live and written up in `docs/spotlight-recipes.md`: place a
  fixture (`vs.Symbol` + select + `Convert To Instrument`), edit `Lighting Device` fields,
  swap fixture type (recreate-and-copy is the only route that works), import symbols from the
  FUSE library, place truss symbols end to end, resize `Straight Truss`, create design and
  sheet layers, create and configure a viewport.
- Not yet tried: hanging positions (`Convert To Hanging Position` / `HP_ConvertToHangPos`),
  Braceworks truss conversion (`ConvertToTruss`), accessories (`LDevice_AddAccessory`),
  editing existing viewports and title blocks on the FUSE template, GDTF modes, 3D rotation,
  label legends, Lightwright exchange. Each of the first two may open a dialog.

## 2. The playground document

Tom saved a disposable copy of the FUSE dev template as
`Z:\TEMPLATES\!!!!!! DEV !!!!!!\gantry-test-playground.vwx`. The name matches the
`gantry-test*` mutation guard in `capability_bench.py`, so mutation cases are allowed on it.

Read back through the bridge on 2026-10-13 (5251 objects):

- 38 design layers in FUSE groups: `[VENUE]`, `[SET]`, `[TRUSS] ...` (CABLE BRIDGES,
  MOTHERGRID, US WALL, SL/SR SIDES, PODS, LED, AIR, FLOOR), `[RIGGING] MOTORS`, `[LX] ...`
  (FLOOR, SCENIC, US WALL, SL/SR SIDES, PODS, AIR, DETAIL 1..3), `[VID] ...`, `[FX] ...`,
  `[KEY] KEY`, `[CL] CENTERS`, plus dashed separator layers.
- 13 sheet layers: `OVER-1..4`, `LX 1..8`, `LX 11`, `LED 1..2`. 87 viewports, 15 title block
  borders.
- The scratch test objects were merged in: 9 `Lighting Device`, 2 `Straight Truss`,
  1 `Light Position Obj`, and the test layers `LX Plot` (design) and `SL-1 Plot` (sheet).
- It is a template, not a populated plot. For swap-with-accessories tests, paste a handful
  of real fixtures from a show file into it first.

If VW is not running on it when the next session starts: open that file in VW, confirm the
"MCP Bridge" palette shows the bridge started, then
`python scripts/vw_run.py --no-tx --code "import vs; print(vs.GetFName())"` must print
`gantry-test-playground.vwx` before any mutation.

## 3. Decision taken (to be written up as an ADR when the catalog exists)

The unit of delivery for Spotlight automation is a **tested recipe script**, not a new MCP
tool.

- Recipes live in `scripts/recipes/` (directory not yet created), one task per file,
  parameterised at the top, run through `scripts/vw_run.py`, verified against the playground
  with a read-back in a second call.
- Only recipes that are used daily and have stable arguments get promoted to typed L2 tools
  in `mcp-server/src/server.rs`. Promotion costs a schema, a Rust rebuild and a bench case,
  so it comes after the recipe has settled.
- A task catalog drives which recipes get written, in frequency order.

## 4. Decision pending: the task catalog, in its own planning session

Tom's call on 2026-10-13: define the list of common tasks, but in a separate planning
session (this session was engineering-heavy; the catalog is a product question about what
FUSE does daily). That session should follow Tom's plan-to-prompts pattern and produce:

1. `docs/task-catalog.md`: one row per task. Columns: task (outcome-level wording, e.g.
   "re-channel a position from a Lightwright export"), inputs, outputs, frequency,
   dialog risk, status (`verified` / `recipe exists` / `untried` / `blocked by dialog`),
   recipe file.
2. One self-contained sub-session prompt per task cluster. Likely clusters: fixtures
   (place, edit, swap, accessories, GDTF), rigging (truss runs, hanging positions, hoists,
   Braceworks), sheets and viewports (per-position sheets, scales, title block fields),
   data import and export (Lightwright, spreadsheets, record formats).
3. `docs/decisions/002-recipes-before-tools.md` recording section 3 and the promotion
   criteria.

Questions only Tom can answer, to settle at the start of that session:

- Where do inputs come from: Lightwright, production spreadsheets, GDTF, the FUSE libraries?
- Who runs this: Tom through Claude Code only, or the department through a chat client?
  This decides how much guardrailing recipes need and whether they must become tools.
- Which FUSE template conventions are fixed (layer naming, sheet naming, viewport scales,
  title block fields)? The playground encodes them and they can be extracted as constraints.
- For dialog-only commands (Replace Instrument, possibly Convert To Hanging Position): design
  around them, or build a UI-automation fallback?
- Does the catalog live in this repo or with the FUSE standards?

## 5. Immediate probes that need no planning (run on the playground, one undo step per call)

These fill in real status for the catalog instead of guesses. Use `transaction=false` for
the first probe of any unknown name so a stderr fragment does not roll back the read-back.

1. **Hanging position from truss.** Select the two `Straight Truss` objects and try
   `vs.DoMenuTextByName('Convert To Hanging Position', 0)`; if it blocks on a dialog,
   dismiss it in VW and try `vs.HP_ConvertToHangPos(h)` instead. Read back
   `(PON='Hanging Position')` and whether a `Lighting Device` with `Position` matching the
   new name gets attached (`LDevice_GetPosition` family, check with `vs_signature`).
2. **Braceworks truss.** `vs.DoMenuTextByName('ConvertToTruss', 0)` on a FUSE truss symbol
   instance. Read back records on the object (`NumRecords` / `GetRecord`).
3. **Accessories.** `vs.LDevice_AddAccessory` on one of the 9 devices (signature first), then
   `read_records_by_format("Lighting Device")` and the accessory record if one appears.
4. **Existing viewports.** Pick one of the 87 viewports (type 122 via `vs.GetTypeN`), read
   object variables 1003/1007/1032 and `GetVPLayerVisibility`, change the scale and a layer
   visibility, `UpdateVP`, read back. Same for a `Title Block Border` record
   (`list_record_formats` to find the field names, then `SetRField`).
5. If any of these opens a dialog: the main thread is blocked until it is dismissed in VW;
   every call times out at 25 s meanwhile. Dismiss it, note the command in
   `docs/spotlight-recipes.md` under "Not covered yet" as `blocked by dialog`, move on.

Record each result in `docs/spotlight-recipes.md` (works, or did not work and how it was
read back) and commit after each green probe.

## 6. Reminders that still apply

- Never test against a non-disposable document. Mutation guard: file name must start with
  `gantry-test`.
- `run_script`: one undo step per call, stderr means failure and rollback, globals do not
  persist, 25 s ceiling, never open dialogs, never `py_mode=execute`.
- Plug-in objects regenerate only after the call returns: mutate in one call, read back in
  the next. `vs.LNewObj()` is per call.
- After `script_crashed` or `engine_poisoned`, restart VW. `scripts/vw_relaunch.ps1 -Force`
  does it on the blank scratch file; for the playground, relaunch by hand.
- Keep LF line endings. Commit after each green step with a message saying what was
  verified. Push to `origin claude/lattice-platform-mcp-TM9xi`. Update `FORK_NOTES.md` with
  session facts (newest section first).
