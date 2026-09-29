#!/usr/bin/python3
"""SG Office -- the spreadsheet ribbon's Home tab as Excel lays it out.

    gen-calc.py ORIGINAL.ui OUT.ui

ORIGINAL is LibreOffice's own Tabbed ribbon for Calc (notebookbar.ui of the
LibreOffice version office.ini pins, MPL-2.0, kept here unchanged as
scalc-notebookbar-<version>.ui); OUT is SG Office's (notebookbar_sgoffice.ui,
also MPL-2.0 as a modification of it), which the payload installs beside it.
The other tabs are LibreOffice's. Home becomes Excel's groups, in Excel's
order:

  Clipboard | Font | Alignment (+ Merge & Center) | Number
  | Styles: Conditional Formatting, Format as Table, cell styles
  | Cells: Insert, Delete, Format | Editing: Sum, Fill, Clear, Sort & Filter, Find & Select

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: MPL-2.0
"""
import sys
import xml.etree.ElementTree as ET

_n = [0]


def prop(obj, name, value, translatable=False):
    p = ET.SubElement(obj, "property", name=name)
    if translatable:
        p.set("translatable", "yes")
    p.text = value
    return p


def packing(child, **kw):
    pk = ET.SubElement(child, "packing")
    for k, v in kw.items():
        prop(pk, k.replace("_", "-"), str(v))


def button(command, label=None, big=True, menu=False, pos=(0, 0), height=2, width=1, icon=None):
    """A toolbox holding one button; returns its <child> for a grid."""
    _n[0] += 1
    child = ET.Element("child")
    box = ET.SubElement(child, "object", {"class": "sfxlo-NotebookbarToolBox", "id": "bxSG%d" % _n[0]})
    prop(box, "visible", "True")
    prop(box, "can-focus", "True")
    prop(box, "valign", "center")
    if big:
        prop(box, "vexpand", "True")
    prop(box, "toolbar-style", "both" if (big or label) else "icons")
    prop(box, "show-arrow", "False")
    if big:
        prop(box, "icon_size", "3")
    bc = ET.SubElement(box, "child")
    b = ET.SubElement(bc, "object", {"class": "GtkMenuToolButton" if menu else "GtkToolButton", "id": "btnSG%d" % _n[0]})
    prop(b, "visible", "True")
    prop(b, "can-focus", "False")
    if label:
        prop(b, "label", label, translatable=True)
    prop(b, "action-name", command)
    if icon:
        prop(b, "icon-name", icon)
    packing(bc, expand="False", homogeneous="False")
    packing(child, left_attach=pos[0], top_attach=pos[1], height=height, width=width)
    return child


def group(gid, label, children, columns):
    """A labelled group (Excel's): its buttons, the label below, a separator after."""
    child = ET.Element("child")
    g = ET.SubElement(child, "object", {"class": "GtkGrid", "id": gid})
    for k, v in (("visible", "True"), ("can-focus", "False"), ("margin-start", "3"), ("margin-end", "6"),
                 ("margin-top", "6"), ("margin-bottom", "6"), ("row-spacing", "3"), ("column-spacing", "3")):
        prop(g, k, v)
    lc = ET.SubElement(g, "child")
    lab = ET.SubElement(lc, "object", {"class": "GtkLabel", "id": "lb" + gid[2:]})
    prop(lab, "visible", "True")
    prop(lab, "can-focus", "False")
    prop(lab, "valign", "end")
    prop(lab, "vexpand", "True")
    prop(lab, "label", label, translatable=True)
    attrs = ET.SubElement(lab, "attributes")
    ET.SubElement(attrs, "attribute", name="style", value="italic")
    ET.SubElement(attrs, "attribute", name="scale", value="0.9")
    packing(lc, left_attach=0, top_attach=2, width=columns)
    for c in children:
        g.append(c)
    sc = ET.SubElement(g, "child")
    sep = ET.SubElement(sc, "object", {"class": "GtkSeparator"})
    prop(sep, "visible", "True")
    prop(sep, "can-focus", "False")
    prop(sep, "orientation", "vertical")
    packing(sc, left_attach=columns, top_attach=0, height=3)
    packing(child, expand="False", fill="True", position=0)
    return child


def find(root, oid):
    for p in root.iter():
        for c in p:
            if c.tag == "child":
                o = c.find("object")
                if o is not None and o.get("id") == oid:
                    return p, c
    raise KeyError(oid)


def packing_value(child, name):
    pk = child.find("packing")
    for p in pk.findall("property") if pk is not None else ():
        if p.get("name") == name:
            return p
    return None


def main(src, out):
    tree = ET.parse(src)
    root = tree.getroot()
    home, _ = find(root, "gdHomeClipboard")          # pmhbHome: the Home tab's groups
    # Number: Conditional Formatting moves to Styles; the rest shifts left
    number_grid = find(root, "gdHomeNumber")[1].find("object")
    _, cf = find(root, "bxHomeConditionalFormatMenu")
    number_grid.remove(cf)
    for c in number_grid.findall("child"):
        la = packing_value(c, "left-attach")
        if la is not None and int(la.text) >= 1:
            la.text = str(int(la.text) - 1)
    # Alignment: Merge & Center at its end, as in Excel
    align_grid = find(root, "gdHomeAlignment")[1].find("object")
    last = max(int(packing_value(c, "left-attach").text) for c in align_grid.findall("child")
               if packing_value(c, "left-attach") is not None)
    for c in align_grid.findall("child"):
        o = c.find("object")
        if o is not None and o.get("class") == "GtkSeparator":
            packing_value(c, "left-attach").text = str(last + 1)
    align_grid.append(button(".uno:ToggleMergeCells", "Merge & Center", pos=(last, 0)))
    # Styles
    cfb = button(".uno:ConditionalFormatMenu", "Conditional Formatting", menu=True, pos=(0, 0))
    styles = group("gdSGHomeStyles", "Styles", [
        cfb,
        button(".uno:InsertCalcTable", "Format as Table", pos=(1, 0), icon="cmd/lc_autoformat.png"),
        button(".uno:DefaultCellStyles", "Normal", big=False, pos=(2, 0), height=1),
        button(".uno:GoodCellStyles", "Good", big=False, pos=(3, 0), height=1),
        button(".uno:BadCellStyles", "Bad", big=False, pos=(2, 1), height=1),
        button(".uno:NeutralCellStyles", "Neutral", big=False, pos=(3, 1), height=1),
    ], 4)
    cells = group("gdSGHomeCells", "Cells", [
        button(".uno:InsertCell", "Insert", pos=(0, 0)),
        button(".uno:DeleteCell", "Delete", pos=(1, 0)),
        button(".uno:FormatCellDialog", "Format", pos=(2, 0)),
    ], 3)
    editing = group("gdSGHomeEditing", "Editing", [
        button(".uno:AutoSum", "AutoSum", big=False, pos=(0, 0), height=1),
        button(".uno:FillDown", "Fill", big=False, pos=(0, 1), height=1),
        button(".uno:Delete", "Clear", big=False, pos=(1, 0), height=1),
        button(".uno:DataFilterAutoFilter", "Sort & Filter", pos=(2, 0)),
        button(".uno:SearchDialog", "Find & Select", pos=(3, 0)),
    ], 4)
    # the Home tab: LibreOffice's Cells group gives way to Styles, Cells, Editing
    kids = [c for c in home if c.tag == "child"]
    old_cells = find(root, "gdHomeCells")[1]
    at = kids.index(old_cells)
    home.remove(old_cells)
    for i, g in enumerate((styles, cells, editing)):
        home.insert(list(home).index(kids[at - 1]) + 1 + i, g)
    for i, c in enumerate(c for c in home if c.tag == "child"):
        p = packing_value(c, "position")
        if p is not None:
            p.text = str(i)
    tree.write(out, encoding="UTF-8", xml_declaration=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
