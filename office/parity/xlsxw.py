"""A small .xlsx writer for the formula corpus.

Writes formulas exactly as Excel stores them in a file: functions newer than
Excel 2007 carry their "_xlfn." (and "_xlws.") prefixes, LET/LAMBDA
parameters "_xlpm.", and a dynamic-array formula is an array formula with
Excel's cell metadata (cm="1" -> XLDAPR "fDynamic"), as Excel 365 writes it.
No cached values: whoever opens the file must calculate.

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import zipfile
from xml.sax.saxutils import escape


def col_name(c):
    s = ""
    c += 1
    while c:
        c, r = divmod(c - 1, 26)
        s = chr(65 + r) + s
    return s


def ref(r, c):
    return f"{col_name(c)}{r + 1}"


class Sheet:
    def __init__(self, name):
        self.name = name
        self.cells = {}          # (r, c) -> (kind, payload)

    def value(self, r, c, v):
        self.cells[(r, c)] = ("v", v)

    def formula(self, r, c, f, array_ref=None, dynamic=False):
        """f without the leading '='."""
        self.cells[(r, c)] = ("f", (f, array_ref, dynamic))


class Workbook:
    def __init__(self):
        self.sheets = []
        self.defined_names = []   # (name, text)

    def sheet(self, name):
        s = Sheet(name)
        self.sheets.append(s)
        return s

    def _sheet_xml(self, sh, strings):
        rows = {}
        for (r, c), cell in sh.cells.items():
            rows.setdefault(r, []).append((c, cell))
        out = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
               '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
               'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">',
               '<sheetData>']
        for r in sorted(rows):
            out.append(f'<row r="{r + 1}">')
            for c, (kind, p) in sorted(rows[r], key=lambda x: x[0]):
                a = ref(r, c)
                if kind == "v":
                    if isinstance(p, bool):
                        out.append(f'<c r="{a}" t="b"><v>{int(p)}</v></c>')
                    elif isinstance(p, (int, float)):
                        out.append(f'<c r="{a}"><v>{repr(float(p)) if isinstance(p, float) else p}</v></c>')
                    elif p is None:
                        pass
                    elif isinstance(p, str) and p.startswith("#") and p.endswith(("!", "?", "A")) and p in (
                            "#N/A", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#NULL!"):
                        out.append(f'<c r="{a}" t="e"><v>{p}</v></c>')
                    else:
                        if p not in strings:
                            strings[p] = len(strings)
                        out.append(f'<c r="{a}" t="s"><v>{strings[p]}</v></c>')
                else:
                    f, aref, dyn = p
                    if aref:
                        cm = ' cm="1"' if dyn else ''
                        out.append(f'<c r="{a}"{cm}><f t="array" ref="{aref}">{escape(f)}</f></c>')
                    else:
                        out.append(f'<c r="{a}"><f>{escape(f)}</f></c>')
            out.append('</row>')
        out.append('</sheetData></worksheet>')
        return "".join(out)

    def save(self, path):
        strings = {}
        sheet_xml = [self._sheet_xml(s, strings) for s in self.sheets]
        any_dynamic = any(k == "f" and p[2] for s in self.sheets for k, p in s.cells.values())
        z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED)
        ct = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
              '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
              '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>',
              '<Default Extension="xml" ContentType="application/xml"/>',
              '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>',
              '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>',
              '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>']
        if any_dynamic:
            ct.append('<Override PartName="/xl/metadata.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheetMetadata+xml"/>')
        for i in range(len(self.sheets)):
            ct.append(f'<Override PartName="/xl/worksheets/sheet{i + 1}.xml" '
                      'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>')
        ct.append('</Types>')
        z.writestr("[Content_Types].xml", "".join(ct))
        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                   '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
                   '</Relationships>')
        wb = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
              '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
              'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>']
        for i, s in enumerate(self.sheets):
            wb.append(f'<sheet name="{escape(s.name)}" sheetId="{i + 1}" r:id="rId{i + 1}"/>')
        wb.append('</sheets>')
        if self.defined_names:
            wb.append('<definedNames>')
            for n, t in self.defined_names:
                wb.append(f'<definedName name="{escape(n)}">{escape(t)}</definedName>')
            wb.append('</definedNames>')
        wb.append('<calcPr calcId="191029" fullCalcOnLoad="1"/></workbook>')
        z.writestr("xl/workbook.xml", "".join(wb))
        rels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
        n = len(self.sheets)
        for i in range(n):
            rels.append(f'<Relationship Id="rId{i + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet{i + 1}.xml"/>')
        rels.append(f'<Relationship Id="rId{n + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>')
        rels.append(f'<Relationship Id="rId{n + 2}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>')
        if any_dynamic:
            rels.append(f'<Relationship Id="rId{n + 3}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sheetMetadata" Target="metadata.xml"/>')
        rels.append('</Relationships>')
        z.writestr("xl/_rels/workbook.xml.rels", "".join(rels))
        for i, x in enumerate(sheet_xml):
            z.writestr(f"xl/worksheets/sheet{i + 1}.xml", x)
        ss = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
              f'<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="{len(strings)}" uniqueCount="{len(strings)}">']
        for s, _ in sorted(strings.items(), key=lambda x: x[1]):
            ss.append(f'<si><t xml:space="preserve">{escape(s)}</t></si>')
        ss.append('</sst>')
        z.writestr("xl/sharedStrings.xml", "".join(ss))
        z.writestr("xl/styles.xml",
                   '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                   '<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
                   '<fonts count="1"><font><sz val="11"/><name val="Calibri"/><family val="2"/></font></fonts>'
                   '<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>'
                   '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
                   '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
                   '<cellXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/></cellXfs>'
                   '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
                   '</styleSheet>')
        if any_dynamic:
            # Excel 365's dynamic-array cell metadata (XLDAPR), as it writes it.
            z.writestr("xl/metadata.xml",
                       '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                       '<metadata xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
                       'xmlns:xda="http://schemas.microsoft.com/office/spreadsheetml/2017/dynamicarray">'
                       '<metadataTypes count="1"><metadataType name="XLDAPR" minSupportedVersion="120000" copy="1" '
                       'pasteAll="1" pasteValues="1" merge="1" splitFirst="1" rowColShift="1" clearFormats="1" '
                       'clearComments="1" assign="1" coerce="1" cellMeta="1"/></metadataTypes>'
                       '<futureMetadata name="XLDAPR" count="1"><bk><extLst><ext uri="{bdbb8cdc-fa1e-496e-a857-3c3f30c029c3}">'
                       '<xda:dynamicArrayProperties fDynamic="1" fCollapsed="0"/></ext></extLst></bk></futureMetadata>'
                       '<cellMetadata count="1"><bk><rc t="1" v="0"/></bk></cellMetadata></metadata>')
        z.close()
