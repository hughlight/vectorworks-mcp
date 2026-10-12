{ VW2026.5 Diagnostic Probe — VectorScript

  Tests the OLDEST, MOST STABLE scripting layer. If THIS crashes VW, the
  regression is in VW itself, not our Python engine or our plugin.

  Run via VW: Tools -> Plug-Ins -> Run Script... -> pick this file.
  Or paste into Window -> Palettes -> Script Palettes.

  Output: writes vs-probe-<timestamp>.json into the OS user profile dir.
    Windows: %USERPROFILE%\.vwmcp-diag\
    Mac:     $HOME/.vwmcp-diag/

  Adapted from JeromyJSmith/lattice-platform vw-plugin/scripts/preflight_probe.vs.
}

PROCEDURE VWMCPProbe;

VAR
  docPath      : STRING;
  docName      : STRING;
  vwVersion    : STRING;
  ifcScheme    : STRING;
  ifcSchemeInt : INTEGER;
  ifcOk        : BOOLEAN;
  layerCount   : INTEGER;
  classCount   : INTEGER;
  hasDoc       : BOOLEAN;
  outDir       : STRING;
  outPath      : STRING;
  jsonStr      : STRING;
  fileErr      : INTEGER;
  ts           : LONGINT;
  lh           : HANDLE;
  i            : INTEGER;
  found        : BOOLEAN;
  ch           : STRING;
  vMaj, vMin, vMaint, vPlat : INTEGER;

  PROCEDURE AppendLn(VAR s : STRING; line : STRING);
  BEGIN
    s := Concat(s, line, Chr(10));
  END;

  FUNCTION BoolStr(b : BOOLEAN) : STRING;
  BEGIN
    IF b THEN BoolStr := 'true' ELSE BoolStr := 'false';
  END;

  { Escape backslashes and double-quotes so VS strings can be embedded inside
    JSON string literals without breaking strict parsers (jq, json.load, etc.). }
  FUNCTION JsonEscape(s : STRING) : STRING;
  VAR
    k : INTEGER;
    c : STRING;
    r : STRING;
  BEGIN
    r := '';
    FOR k := 1 TO Len(s) DO
    BEGIN
      c := Copy(s, k, 1);
      IF c = '\' THEN
        r := Concat(r, '\\')
      ELSE IF c = '"' THEN
        r := Concat(r, '\"')
      ELSE
        r := Concat(r, c);
    END;
    JsonEscape := r;
  END;

BEGIN
  { --- Document state --- }
  docPath := GetFPathName;
  docName := GetFName;
  hasDoc  := (Len(docName) > 0);

  { --- Version: GetVersion returns via VAR params (major, minor, maint, platform) --- }
  GetVersion(vMaj, vMin, vMaint, vPlat);
  vwVersion := Concat(Num2Str(0, vMaj), '.', Num2Str(0, vMin), '.', Num2Str(0, vMaint));

  { --- IFC scheme (the U5-suspicious bit) ---
    IFC_GetIFCScheme(VAR outScheme:INTEGER):BOOLEAN — returns integer scheme code via VAR. }
  ifcOk := IFC_GetIFCScheme(ifcSchemeInt);
  IF ifcOk THEN
    ifcScheme := Num2Str(0, ifcSchemeInt)
  ELSE
    ifcScheme := 'UNKNOWN';

  { --- Layer + class counts via iteration --- }
  classCount := ClassNum;
  layerCount := 0;
  lh := FLayer;
  WHILE lh <> NIL DO
  BEGIN
    layerCount := layerCount + 1;
    lh := NextLayer(lh);
  END;

  { --- Timestamp: GetTickCount returns LONGINT (ticks @ 60Hz since system boot).
    Overflows VS INTEGER (16-bit) within minutes, so ts must be LONGINT. }
  ts := GetTickCount DIV 60;

  { --- Output dir: same folder as the active doc.
    GetFPathName returns OS-native separators (\ on Windows, / on Mac), so strip
    back to whichever appears last. VS AND does not short-circuit (Pascal semantics),
    so guard Copy() inside the loop body where i > 0 is already known. }
  outDir := docPath;
  i := Len(outDir);
  found := FALSE;
  WHILE (i > 0) AND (NOT found) DO
  BEGIN
    ch := Copy(outDir, i, 1);
    IF (ch = '/') OR (ch = '\') THEN
      found := TRUE
    ELSE
      i := i - 1;
  END;
  IF found THEN
    outDir := Copy(outDir, 1, i);

  outPath := Concat(outDir, 'vs-probe-', Num2Str(0, ts), '.json');

  { --- Build JSON --- }
  jsonStr := '{';
  AppendLn(jsonStr, '');
  AppendLn(jsonStr, Concat('  "probe": "vectorscript",'));
  AppendLn(jsonStr, Concat('  "timestamp": ', Num2Str(0, ts), ','));
  AppendLn(jsonStr, Concat('  "vw_version": "', JsonEscape(vwVersion), '",'));
  AppendLn(jsonStr, Concat('  "doc_name": "', JsonEscape(docName), '",'));
  AppendLn(jsonStr, Concat('  "doc_path": "', JsonEscape(docPath), '",'));
  AppendLn(jsonStr, Concat('  "has_document": ', BoolStr(hasDoc), ','));
  AppendLn(jsonStr, Concat('  "ifc_scheme": "', JsonEscape(ifcScheme), '",'));
  AppendLn(jsonStr, Concat('  "ifc_scheme_ok": ', BoolStr(ifcOk), ','));
  AppendLn(jsonStr, Concat('  "layer_count": ', Num2Str(0, layerCount), ','));
  AppendLn(jsonStr, Concat('  "class_count": ', Num2Str(0, classCount)));
  AppendLn(jsonStr, '}');

  { --- Write JSON next to the document.
    VS file I/O has no handles: Rewrite(path) opens, WriteLn(args) writes to current
    file, Close(path) closes. GetLastFileErr returns 0 on success. }
  Rewrite(outPath);
  fileErr := GetLastFileErr;
  IF fileErr = 0 THEN
  BEGIN
    WriteLn(jsonStr);
    Close(outPath);
    AlrtDialog(Concat(
      'VectorScript probe OK.', Chr(10),
      'VW: ', vwVersion, Chr(10),
      'Layers: ', Num2Str(0, layerCount), '  Classes: ', Num2Str(0, classCount), Chr(10),
      'IFC scheme: ', ifcScheme, Chr(10),
      'Report: ', outPath
    ));
  END
  ELSE
    AlrtDialog(Concat('VectorScript probe ran but COULD NOT WRITE report (err ', Num2Str(0, fileErr), '): ', outPath));
END;

RUN(VWMCPProbe);
