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
  layerCount   : INTEGER;
  classCount   : INTEGER;
  hasDoc       : BOOLEAN;
  outDir       : STRING;
  outPath      : STRING;
  jsonStr      : STRING;
  fileRef      : INTEGER;
  homeEnv      : STRING;
  ts           : INTEGER;
  lh           : HANDLE;

  PROCEDURE AppendLn(VAR s : STRING; line : STRING);
  BEGIN
    s := Concat(s, line, Chr(10));
  END;

BEGIN
  { --- Document state --- }
  docPath := GetFPathName;
  docName := GetFName;
  hasDoc  := (Length(docName) > 0);

  { --- Version --- }
  vwVersion := GetVWVersion;

  { --- IFC scheme (the U5-suspicious bit) --- }
  ifcScheme := IFC_GetIFCScheme;
  IF Length(ifcScheme) = 0 THEN ifcScheme := 'UNKNOWN';

  { --- Layer + class counts via iteration --- }
  classCount := ClassNum;
  layerCount := 0;
  lh := FLayer;
  WHILE lh <> NIL DO
  BEGIN
    layerCount := layerCount + 1;
    lh := NextLayer(lh);
  END;

  { --- Timestamp (epoch seconds approximation; VS has no proper time API) --- }
  ts := Round((GetTickCount / 1000));

  { --- Output dir: ~/.vwmcp-diag/ --- }
  { Both Windows and Mac return POSIX-style HOME via env. Fall back to doc dir. }
  homeEnv := '';
  { VS has no getenv; we hardcode reasonable defaults. The Python probe handles env. }

  outDir := docPath;
  { strip filename to leave directory (POSIX path since VW2019) }
  VAR i : INTEGER;
  FOR i := Len(outDir) DOWNTO 1 DO
  BEGIN
    IF Copy(outDir, i, 1) = '/' THEN
    BEGIN
      outDir := Copy(outDir, 1, i);
      i := 0;
    END;
  END;

  outPath := Concat(outDir, 'vs-probe-', Num2Str(0, ts), '.json');

  { --- Build JSON --- }
  jsonStr := '{';
  AppendLn(jsonStr, '');
  AppendLn(jsonStr, Concat('  "probe": "vectorscript",'));
  AppendLn(jsonStr, Concat('  "timestamp": ', Num2Str(0, ts), ','));
  AppendLn(jsonStr, Concat('  "vw_version": "', vwVersion, '",'));
  AppendLn(jsonStr, Concat('  "doc_name": "', docName, '",'));
  AppendLn(jsonStr, Concat('  "doc_path": "', docPath, '",'));
  AppendLn(jsonStr, Concat('  "has_document": ', BooleanToStr(hasDoc), ','));
  AppendLn(jsonStr, Concat('  "ifc_scheme": "', ifcScheme, '",'));
  AppendLn(jsonStr, Concat('  "ifc_scheme_ok": ', BooleanToStr(ifcScheme = 'IFC4X3'), ','));
  AppendLn(jsonStr, Concat('  "layer_count": ', Num2Str(0, layerCount), ','));
  AppendLn(jsonStr, Concat('  "class_count": ', Num2Str(0, classCount)));
  AppendLn(jsonStr, '}');

  { --- Write JSON next to the document --- }
  fileRef := FOpen(outPath, 'w');
  IF fileRef > 0 THEN
  BEGIN
    WriteLn(fileRef, jsonStr);
    FClose(fileRef);
    AlrtDialog(Concat(
      'VectorScript probe OK.', Chr(10),
      'VW: ', vwVersion, Chr(10),
      'Layers: ', Num2Str(0, layerCount), '  Classes: ', Num2Str(0, classCount), Chr(10),
      'IFC scheme: ', ifcScheme, Chr(10),
      'Report: ', outPath
    ));
  END
  ELSE
    AlrtDialog(Concat('VectorScript probe ran but COULD NOT WRITE report to: ', outPath));
END;

RUN(VWMCPProbe);
