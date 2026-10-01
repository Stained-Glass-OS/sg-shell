#!/usr/bin/python3
"""SG Office -- its registrations in the Windows registry, as two .reg files
sg-prefix-init imports into the system prefix (defaults.d):

  89-sg-office.reg        the programs, their ProgIDs, verbs and icons, "Open
                          with", App Paths, Default apps' capabilities. Safe to
                          change: re-importing it only refreshes our own keys.
  89-sg-office-types.reg  which program each file type opens with (.docx ->
                          SG Office ...). NEVER CHANGE IT once shipped:
                          sg-prefix-init re-imports a defaults file whenever
                          its contents change, and re-importing this one would
                          take the file types back from a suite the user
                          installed later (Microsoft Office sets them to its
                          own ProgIDs when it installs; the user switches back
                          in Settings > Default apps).

    gen-reg.py OUTDIR

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys

DIR = "Z:\\\\usr\\\\libexec\\\\stained-glass\\\\shell\\\\"
EXE = {"documents": "sg-documents64.exe", "spreadsheets": "sg-spreadsheets64.exe",
       "presentations": "sg-presentations64.exe"}
APP = {"documents": "SG Office Documents", "spreadsheets": "SG Office Spreadsheets",
       "presentations": "SG Office Presentations"}

# ProgID: (program, friendly type name, extensions, MIME type of the first, open verb extra)
TYPES = [
    ("SGOffice.Document.12", "documents", "Document", [".docx", ".docm"],
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document"),
    ("SGOffice.Document.8", "documents", "Document (97-2003)", [".doc"], "application/msword"),
    ("SGOffice.DocumentTemplate", "documents", "Document Template", [".dotx", ".dotm", ".dot"],
     "application/vnd.openxmlformats-officedocument.wordprocessingml.template"),
    ("SGOffice.OpenDocumentText", "documents", "OpenDocument Text", [".odt", ".ott"],
     "application/vnd.oasis.opendocument.text"),
    ("SGOffice.Sheet.12", "spreadsheets", "Spreadsheet", [".xlsx", ".xlsm", ".xlsb"],
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
    ("SGOffice.Sheet.8", "spreadsheets", "Spreadsheet (97-2003)", [".xls"], "application/vnd.ms-excel"),
    ("SGOffice.SheetTemplate", "spreadsheets", "Spreadsheet Template", [".xltx", ".xltm", ".xlt"],
     "application/vnd.openxmlformats-officedocument.spreadsheetml.template"),
    ("SGOffice.OpenDocumentSpreadsheet", "spreadsheets", "OpenDocument Spreadsheet", [".ods", ".ots"],
     "application/vnd.oasis.opendocument.spreadsheet"),
    ("SGOffice.CSV", "spreadsheets", "Comma Separated Values", [".csv"], "text/csv"),
    ("SGOffice.Show.12", "presentations", "Presentation", [".pptx", ".pptm"],
     "application/vnd.openxmlformats-officedocument.presentationml.presentation"),
    ("SGOffice.Show.8", "presentations", "Presentation (97-2003)", [".ppt"], "application/vnd.ms-powerpoint"),
    ("SGOffice.SlideShow", "presentations", "Slide Show", [".ppsx", ".ppsm", ".pps"],
     "application/vnd.openxmlformats-officedocument.presentationml.slideshow"),
    ("SGOffice.ShowTemplate", "presentations", "Presentation Template", [".potx", ".potm", ".pot"],
     "application/vnd.openxmlformats-officedocument.presentationml.template"),
    ("SGOffice.OpenDocumentPresentation", "presentations", "OpenDocument Presentation", [".odp", ".otp"],
     "application/vnd.oasis.opendocument.presentation"),
]
# File Explorer's New menu: an empty file of the type opens as a new document
NEW = {".docx": "SGOffice.Document.12", ".xlsx": "SGOffice.Sheet.12", ".pptx": "SGOffice.Show.12"}

HEADER = "Windows Registry Editor Version 5.00\n\n"


def q(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def programs():
    out = [HEADER, "; Stained Glass OS -- SG Office: its programs and file types (office/gen-reg.py).\n"
                   "; Which program a type opens with by default is 89-sg-office-types.reg.\n"]
    for kind, exe in EXE.items():
        path = DIR + exe
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s]\n@="%s"\n'
                   % (exe.replace("64", ""), path))
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\Applications\\%s]\n"FriendlyAppName"="%s"\n' % (exe, APP[kind]))
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\Applications\\%s\\shell\\open\\command]\n@="\\"%s\\" \\"%%1\\""\n'
                   % (exe, path))
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\Applications\\%s\\SupportedTypes]\n' % exe
                   + "".join('"%s"=""\n' % e for p, k, n, exts, m in TYPES if k == kind for e in exts))
    for progid, kind, name, exts, mime in TYPES:
        path = DIR + EXE[kind]
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s]\n@="%s"\n"FriendlyTypeName"="%s"\n' % (progid, name, name))
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s\\DefaultIcon]\n@="%s,1"\n' % (progid, path))
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s\\shell]\n@="open"\n' % progid)
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s\\shell\\open]\n@="&Open"\n' % progid)
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s\\shell\\open\\command]\n@="\\"%s\\" \\"%%1\\""\n'
                   % (progid, path))
        # no Print verb: the editors print from their File menu, not from a
        # command line (a /p from an older registration opens the file)
        for e in exts:
            # in "Open with" even when another program is the default
            out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s\\OpenWithProgids]\n"%s"=""\n' % (e, progid))
    for e, progid in NEW.items():
        out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s\\%s\\ShellNew]\n"NullFile"=""\n' % (e, progid))
    # Default apps: SG Office as a registered application with its capabilities
    cap = "Software\\\\Stained Glass\\\\SG Office\\\\Capabilities"
    out.append('[HKEY_LOCAL_MACHINE\\Software\\RegisteredApplications]\n"SG Office"="%s"\n' % cap)
    out.append('[HKEY_LOCAL_MACHINE\\Software\\Stained Glass\\SG Office\\Capabilities]\n'
               '"ApplicationName"="SG Office"\n'
               '"ApplicationDescription"="Documents, spreadsheets and presentations, in Microsoft Office\'s formats."\n')
    out.append('[HKEY_LOCAL_MACHINE\\Software\\Stained Glass\\SG Office\\Capabilities\\FileAssociations]\n'
               + "".join('"%s"="%s"\n' % (e, p) for p, k, n, exts, m in TYPES for e in exts))
    return "\n".join(out)


def types():
    out = [HEADER, "; Stained Glass OS -- SG Office: which program each file type opens with.\n"
                   ";\n"
                   "; NEVER CHANGE THIS FILE once shipped (see office/gen-reg.py): a changed\n"
                   "; file is imported again, and would take the types back from a suite the\n"
                   "; user installed since (Microsoft Office takes them when it installs).\n"]
    for progid, kind, name, exts, mime in TYPES:
        for i, e in enumerate(exts):
            ct = '"Content Type"="%s"\n' % mime if i == 0 else ""
            out.append('[HKEY_LOCAL_MACHINE\\Software\\Classes\\%s]\n@="%s"\n%s' % (e, progid, ct))
    return "\n".join(out)


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "89-sg-office.reg"), "w", encoding="utf-8") as f:
        f.write(programs())
    with open(os.path.join(outdir, "89-sg-office-types.reg"), "w", encoding="utf-8") as f:
        f.write(types())


if __name__ == "__main__":
    main(sys.argv[1])
