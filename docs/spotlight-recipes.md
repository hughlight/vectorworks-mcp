# Spotlight recipes through run_script

Verified live on 2026-10-12 against Vectorworks Spotlight 2026 (31.7.0.879357) on the scratch
copy of `TEST BLANK FILE.vwx`, which already carries 49 FUSE symbols (ACME Supernova, Chauvet
Color Strike M, GLP bars, FUSE title block). Every recipe below was run through
`scripts/vw_run.py` and read back in a second call. "Did not work" means tried and read back
unchanged, not untested.

Read `docs/agent-guide.md` first. Four rules from these probes that apply to every Spotlight
script:

1. **Plug-in objects regenerate after the script returns.** `vs.ResetObject(h)` inside a call
   does not update geometry or derived fields until the call ends. Mutate in one call, read back
   in the next.
2. **`vs.LNewObj()` is per call.** In a fresh call it returns a null handle. Find objects again
   with criteria, for example `vs.ForEachObject(cb, "(PON='Lighting Device')")`.
3. **A failed `vs.*` call writes a traceback fragment to stderr without raising.** Unknown
   plug-in names in `CreateCustomObjectN`, unknown menu names in `DoMenuTextByName`, and
   `GetBBox` on an empty plug-in object all do this. With `transaction=true` the whole call rolls
   back. Use `transaction=false` for exploratory probes, or validate names first.
4. **Menu commands by universal name.** `vs.DoMenuTextByName(name, 0)` takes the universal
   name from `C:\Program Files\Vectorworks 2026\Workspaces\Spotlight.vww` with `_x20` read as a
   space, case-insensitive. Any command that opens a dialog will block the main thread; only use
   ones known to run silently.

## Place a lighting device

A bare `CreateCustomObjectN('Lighting Device', ...)` with `Symbol Name` set never becomes a
real fixture (no Inst Type, no wattage). Insert the fixture symbol, select it, convert it:

```python
import vs, json
SYM = 'ACME SUPERNOVA - AIR'          # must exist in the document; see "Import from a library"
vs.DSelectAll()
vs.Symbol(SYM, (0, -21000), 0)       # x, y in document units, rotation in degrees
vs.SetSelect(vs.LNewObj())
vs.DoMenuTextByName('Convert To Instrument', 0)   # silent; universal name from Spotlight.vww
vs.DSelectAll()
print(json.dumps({"placed": SYM}))
```

Read back in the next call: the object is type 86 with the `Lighting Device` record,
`Inst Type` = `ACME SUPERNOVA`, `Wattage` = `1800`, `Weight` = `51.50 lb`, all pulled from the
symbol's Light Info Record.

## Find lighting devices

```python
import vs, json
rows = []
def cb(h):
    x, y = vs.GetSymLoc(h)
    rows.append({"x": x, "y": y, "rot": vs.GetSymRot(h),
                 **{f: vs.GetRField(h, 'Lighting Device', f)
                    for f in ['Inst Type', 'Symbol Name', 'Channel', 'Dimmer', 'Purpose', 'Position', 'Unit Number', 'Color', 'Focus', 'Wattage', 'Weight']}})
vs.ForEachObject(cb, "(PON='Lighting Device')")
print(json.dumps(rows))
```

`read_records_by_format("Lighting Device")` does the same through the L2 tool. Criteria
`(Y=-21000)` style position filters did not match plug-in objects; filter in Python instead.

## Change fixture parameters

```python
import vs
h = ...  # from the finder above
for f, v in [('Channel', '101'), ('Dimmer', '12'), ('Purpose', 'KEY'), ('Unit Number', '7'),
             ('Position', 'LX1'), ('Color', 'R02'), ('Focus', 'DSC')]:
    vs.SetRField(h, 'Lighting Device', f, v)
vs.ResetObject(h)
```

All seven read back after the call and the label legend regenerated. The record has 159
fields; `list_record_formats` shows them. Derived fields (`Inst Type`, `Wattage`, `Weight`) are
read-only in practice.

## Swap a fixture type

Did not work (read back unchanged): setting `Symbol Name`, setting `Symbol Definition` (it
rewrites the stored name but not the fixture), `LDevice_SetParamStr(h, -1, -2, 'Symbol Name', ...)`,
`Use Active Symbol` = True, `kReplaceWithActSymButton` = 1, each followed by `ResetObject` or
`LDevice_Reset`. The UI's Replace Instrument command opens a dialog.

What works: recreate the device from the new symbol at the same place and copy the data
fields over.

```python
import vs, json
NEW = 'CHAUVET COLOR STRIKE M - AIR'
FIELDS = ['Channel', 'Dimmer', 'Purpose', 'Unit Number', 'Position', 'Color', 'Focus']
old = ...  # handle from the finder
keep = {f: vs.GetRField(old, 'Lighting Device', f) for f in FIELDS}
px, py = vs.GetSymLoc(old); rot = vs.GetSymRot(old)
vs.DelObject(old)
vs.DSelectAll(); vs.Symbol(NEW, (px, py), rot); vs.SetSelect(vs.LNewObj())
vs.DoMenuTextByName('Convert To Instrument', 0); vs.DSelectAll()
new = [None]
def cb(h):
    x, y = vs.GetSymLoc(h)
    if abs(x - px) < 1 and abs(y - py) < 1: new[0] = h
vs.ForEachObject(cb, "(PON='Lighting Device')")
for f, v in keep.items(): vs.SetRField(new[0], 'Lighting Device', f, v)
vs.ResetObject(new[0])
print(json.dumps({"swapped_to": NEW, "kept": keep}))
```

Read back: `Inst Type` = `Chauvet Color Strike M`, `Wattage` = `870 W`, data fields intact.
Caveats: the handle changes, accessories and 3D rotation are not carried (extend `FIELDS` as
needed), and the whole thing is one undo step when `transaction=true`.

## Import a symbol from a library file

```python
import vs, os, json
lib = os.path.join(os.environ['APPDATA'], 'Nemetschek', 'Vectorworks', '2026', 'Libraries',
                   'FUSE FIXURES AND TRUSS - LOCAL.vwx')
lid, n = vs.BuildResourceListN(16, lib)                     # 16 = symbol definitions; 845 here
names = [vs.GetNameFromResourceList(lid, i) for i in range(1, n + 1)]
want = 'T - FUSE PRE RIG TRUSS 08'
idx = names.index(want) + 1
hdef = vs.ImportResourceToCurrentFile(lid, idx)            # no dialog, returns the definition
print(json.dumps({"imported": vs.GetName(hdef), "library_symbols": n}))
```

Reading a library with `BuildResourceListN` does not modify it. `BuildResourceList2(16, 13, '', True)`
lists the default content set (2580 symbols on this machine); folder indices 0 and 1 list the
current document.

## Place truss

FUSE truss are symbols: import as above, then `vs.Symbol(want, (x, y), 0)`. The 8 ft pre-rig
truss instance has a bbox 98 in wide; place the next one at `x + width` for a run. The symbol
carries no Spotlight truss record, so Braceworks will not see it unless converted
(`ConvertToTruss` universal name exists; not tried because it may open a dialog).

The stock parametric truss works without symbols:

```python
import vs
t = vs.CreateCustomObjectN('Straight Truss', (0, -30000), 0, False)
vs.SetRField(t, 'Straight Truss', 'LineLength', "10'")   # dimension fields take a unit string
vs.ResetObject(t)
```

Bbox was 120 in after the call. Other fields: `Profile` (Box), `Connection` (Plate),
`Height`, `Width`, `Chord Width`, `Hanging Angle`, `Rotation`. `Curved Truss`, `Lighting Pipe`,
`Light Position Obj` and `Soft Goods` also instantiate; `Truss`, `Hoist`, `Lighting Position`
and `Focus Point` are not plug-in names on this install (each wrote a stderr fragment).

## Design and sheet layers

```python
import vs, json
dl = vs.CreateLayer('LX Plot', 1)      # 1 = design layer; makes it active
sl = vs.CreateLayer('SL-1 Plot', 2)    # 2 = sheet layer; makes it active
vs.Layer('LX Plot')                    # switch active layer by name
print(json.dumps({"design": vs.GetLName(dl), "sheet": vs.GetLName(sl),
                  "kind": [vs.GetObjectVariableInt(dl, 154), vs.GetObjectVariableInt(sl, 154)]}))
```

Object variable 154 reads 1 for design, 2 for sheet. `CreateLayer` leaves the new layer active,
so restore the active layer before placing objects.

## Viewports

```python
import vs, json
sl = vs.GetObject('SL-1 Plot'); dl = vs.GetObject('LX Plot')
vp = vs.CreateVP(sl)                                  # type 122
vs.SetVPLayerVisibility(vp, dl, 0)                    # 0 = visible, 2 = grayed, -1 = invisible
vs.SetObjectVariableReal(vp, 1003, 1.0 / 48.0)        # scale 1/4" = 1'
vs.SetObjectVariableInt(vp, 1007, 6)                  # standard view (6 = top/plan here; 7 was the default)
vs.SetObjectVariableString(vp, 1032, 'LX plot 1/4in') # viewport name
vs.Layer('SL-1 Plot'); vs.Rect(-5000, -5000, 5000, 5000)
vs.SetVPCropObject(vp, vs.LNewObj())                  # True
vs.UpdateVP(vp)
print(json.dumps({"scale": vs.GetObjectVariableReal(vp, 1003), "name": vs.GetObjectVariableString(vp, 1032),
                  "lx_visible": vs.GetVPLayerVisibility(vp, dl)}))
```

Everything above read back as set. Object variable 1000 read 6 (projection) and 1001 0
(render mode) on the new viewport; `SetVPClassVisibility` and the `SetVPClOvrd*` family exist
for class overrides (not exercised).

## Not covered yet

- Hanging positions from truss (`HP_ConvertToHangPos`, `Convert To Hanging Position`) and
  attaching devices to a position by geometry rather than the `Position` text field.
- Braceworks truss conversion (`ConvertToTruss`) and hoists.
- Accessories (`LDevice_AddAccessory`), GDTF modes, 3D rotation of fixtures.
- Label legends, Lightwright exchange, Vision export.
- Any command that opens a dialog. If a real playground file is open, these are the next
  things to try.
