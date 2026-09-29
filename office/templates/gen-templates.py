#!/usr/bin/python3
"""SG Office -- the default templates, written as ODF by hand (our own work,
nothing of Microsoft's): what a new document, spreadsheet or presentation
starts from, laid out as people who know Microsoft's suite expect.

    gen-templates.py OUTDIR    writes OUTDIR/{normal.ott,book.ots,blank.otp}

- Documents: body text Calibri 11 pt (drawn with Carlito, its metric-compatible
  open font, keeping the name so documents stay the same on other machines),
  line spacing 1.08, 8 pt after each paragraph; headings Calibri Light in
  blue; 2.54 cm (1 inch) margins. The page size is left to the locale
  (Letter in the US and Canada, A4 elsewhere).
- Spreadsheets: Calibri 11 pt, columns 64 px (8.43 characters) wide, rows
  15 pt high, one sheet named Sheet1.
- Presentations: 16:9 (33.867 x 19.05 cm), titles Calibri Light 44 pt, body
  Calibri 28/24/20/18 pt.

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys
import zipfile

NS = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
      'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
      'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
      'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
      'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
      'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
      'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
      'xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0" '
      'xmlns:meta="urn:oasis:names:tc:opendocument:xmlns:meta:1.0" '
      'xmlns:dc="http://purl.org/dc/elements/1.1/" '
      'xmlns:loext="urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0" '
      'office:version="1.3"')

BODY = "Calibri"
HEAD = "Calibri Light"
BLUE = "#1f4e79"     # our heading blue
BLUE2 = "#2e74b5"

FONT_DECLS = ('<office:font-face-decls>'
              '<style:font-face style:name="Calibri" svg:font-family="Calibri" style:font-family-generic="swiss" style:font-pitch="variable"/>'
              '<style:font-face style:name="Calibri Light" svg:font-family="&apos;Calibri Light&apos;" style:font-family-generic="swiss" style:font-pitch="variable"/>'
              '</office:font-face-decls>')


def text_props(font, size, color=None, bold=False):
    s = ('style:font-name="%s" fo:font-size="%s" style:font-name-asian="%s" style:font-size-asian="%s" '
         'style:font-name-complex="%s" style:font-size-complex="%s"' % (font, size, font, size, font, size))
    if color:
        s += ' fo:color="%s"' % color
    if bold:
        s += ' fo:font-weight="bold" style:font-weight-asian="bold" style:font-weight-complex="bold"'
    return "<style:text-properties %s/>" % s


def manifest(mime, extra=()):
    files = ["content.xml", "styles.xml", "meta.xml", "settings.xml"] + list(extra)
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.3">',
           '<manifest:file-entry manifest:full-path="/" manifest:version="1.3" manifest:media-type="%s"/>' % mime]
    for f in files:
        out.append('<manifest:file-entry manifest:full-path="%s" manifest:media-type="text/xml"/>' % f)
    out.append('</manifest:manifest>')
    return "\n".join(out)


def meta(title):
    return ('<?xml version="1.0" encoding="UTF-8"?><office:document-meta %s><office:meta>'
            '<meta:generator>SG Office templates</meta:generator><dc:title>%s</dc:title>'
            '</office:meta></office:document-meta>' % (NS, title))


SETTINGS = '<?xml version="1.0" encoding="UTF-8"?><office:document-settings %s/>' % NS.replace(
    'office:version="1.3"', 'xmlns:config="urn:oasis:names:tc:opendocument:xmlns:config:1.0" office:version="1.3"')


def write(path, mime, content, styles, title):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), mime, compress_type=zipfile.ZIP_STORED)
        for name, data in (("content.xml", content), ("styles.xml", styles), ("meta.xml", meta(title)),
                           ("settings.xml", SETTINGS), ("META-INF/manifest.xml", manifest(mime))):
            z.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)


# ---- Documents -----------------------------------------------------------------------------------
def heading(name, display, size, color, level, before, after, font=HEAD, bold=False):
    return ('<style:style style:name="%s" style:display-name="%s" style:family="paragraph" '
            'style:parent-style-name="Heading" style:next-style-name="Text_20_body" style:default-outline-level="%d" '
            'style:class="text"><style:paragraph-properties fo:margin-top="%s" fo:margin-bottom="%s" '
            'fo:line-height="108%%" fo:keep-with-next="always"/>%s</style:style>'
            % (name, display, level, before, after, text_props(font, size, color, bold)))


def writer():
    styles = ('<?xml version="1.0" encoding="UTF-8"?><office:document-styles %s>%s<office:styles>'
              '<style:default-style style:family="paragraph">'
              '<style:paragraph-properties fo:margin-top="0cm" fo:margin-bottom="0.282cm" fo:line-height="108%%" '
              'style:tab-stop-distance="1.27cm" style:writing-mode="page"/>%s'
              '</style:default-style>'
              '<style:style style:name="Standard" style:family="paragraph" style:class="text">'
              '<style:paragraph-properties fo:margin-top="0cm" fo:margin-bottom="0.282cm" fo:line-height="108%%"/>'
              '%s</style:style>'
              '<style:style style:name="Text_20_body" style:display-name="Body Text" style:family="paragraph" '
              'style:parent-style-name="Standard" style:class="text"/>'
              '<style:style style:name="Heading" style:family="paragraph" style:parent-style-name="Standard" '
              'style:next-style-name="Text_20_body" style:class="text">%s</style:style>'
              '%s%s%s'
              '<style:style style:name="Title" style:family="paragraph" style:parent-style-name="Heading" '
              'style:next-style-name="Text_20_body" style:class="chapter">'
              '<style:paragraph-properties fo:margin-top="0cm" fo:margin-bottom="0cm" fo:line-height="100%%"/>'
              '%s</style:style>'
              '<style:style style:name="Subtitle" style:family="paragraph" style:parent-style-name="Heading" '
              'style:next-style-name="Text_20_body" style:class="chapter">%s</style:style>'
              '</office:styles>'
              '<office:automatic-styles><style:page-layout style:name="pm1"><style:page-layout-properties '
              'fo:margin-top="2.54cm" fo:margin-bottom="2.54cm" fo:margin-left="2.54cm" fo:margin-right="2.54cm" '
              'style:print-orientation="portrait"/></style:page-layout></office:automatic-styles>'
              '<office:master-styles><style:master-page style:name="Standard" style:page-layout-name="pm1"/>'
              '</office:master-styles></office:document-styles>'
              % (NS, FONT_DECLS, text_props(BODY, "11pt"), text_props(BODY, "11pt"), text_props(HEAD, "16pt", BLUE),
                 heading("Heading_20_1", "Heading 1", "16pt", BLUE2, 1, "0.423cm", "0cm"),
                 heading("Heading_20_2", "Heading 2", "13pt", BLUE2, 2, "0.071cm", "0cm"),
                 heading("Heading_20_3", "Heading 3", "12pt", BLUE, 3, "0.071cm", "0cm"),
                 text_props(HEAD, "28pt"), text_props(BODY, "11pt", "#595959")))
    content = ('<?xml version="1.0" encoding="UTF-8"?><office:document-content %s>%s<office:body><office:text>'
               '<text:p text:style-name="Standard"/></office:text></office:body></office:document-content>'
               % (NS, FONT_DECLS))
    return content, styles


# ---- Spreadsheets --------------------------------------------------------------------------------
def calc():
    styles = ('<?xml version="1.0" encoding="UTF-8"?><office:document-styles %s>%s<office:styles>'
              '<style:default-style style:family="table-cell"><style:paragraph-properties style:tab-stop-distance="1.25cm"/>'
              '%s</style:default-style>'
              '<style:style style:name="Default" style:family="table-cell">%s</style:style>'
              '</office:styles></office:document-styles>'
              % (NS, FONT_DECLS, text_props(BODY, "11pt"), text_props(BODY, "11pt")))
    content = ('<?xml version="1.0" encoding="UTF-8"?><office:document-content %s>%s'
               '<office:automatic-styles>'
               '<style:style style:name="co1" style:family="table-column"><style:table-column-properties '
               'fo:break-before="auto" style:column-width="2.258cm"/></style:style>'
               '<style:style style:name="ro1" style:family="table-row"><style:table-row-properties '
               'style:row-height="0.529cm" fo:break-before="auto" style:use-optimal-row-height="true"/></style:style>'
               '<style:style style:name="ta1" style:family="table" style:master-page-name="Default">'
               '<style:table-properties table:display="true" style:writing-mode="lr-tb"/></style:style>'
               '</office:automatic-styles><office:body><office:spreadsheet>'
               '<table:table table:name="Sheet1" table:style-name="ta1">'
               '<table:table-column table:style-name="co1" table:number-columns-repeated="16384" '
               'table:default-cell-style-name="Default"/>'
               '<table:table-row table:style-name="ro1" table:number-rows-repeated="1048576">'
               '<table:table-cell table:number-columns-repeated="16384"/></table:table-row>'
               '</table:table></office:spreadsheet></office:body></office:document-content>'
               % (NS, FONT_DECLS))
    return content, styles


# ---- Presentations -------------------------------------------------------------------------------
def impress():
    def pstyle(name, font, size, extra=""):
        # no fill, no line: text on the slide, as Office's placeholders (LibreOffice's
        # default drawing style would paint them blue)
        graphic = extra or '<style:graphic-properties draw:fill="none" draw:stroke="none"/>'
        return ('<style:style style:name="Default-%s" style:family="presentation">%s%s</style:style>'
                % (name, graphic, text_props(font, size)))
    outline = "".join(pstyle("outline%d" % i, BODY, s) for i, s in ((1, "28pt"), (2, "24pt"), (3, "20pt"),
                                                                     (4, "18pt"), (5, "18pt"), (6, "18pt"),
                                                                     (7, "18pt"), (8, "18pt"), (9, "18pt")))
    styles = ('<?xml version="1.0" encoding="UTF-8"?><office:document-styles %s>%s<office:styles>'
              '<style:default-style style:family="graphic">%s</style:default-style>'
              '%s%s%s'
              '</office:styles><office:automatic-styles>'
              '<style:page-layout style:name="PM1"><style:page-layout-properties fo:margin-top="0cm" '
              'fo:margin-bottom="0cm" fo:margin-left="0cm" fo:margin-right="0cm" fo:page-width="33.867cm" '
              'fo:page-height="19.05cm" style:print-orientation="landscape"/></style:page-layout>'
              '<style:style style:name="Mdp1" style:family="drawing-page"><style:drawing-page-properties '
              'draw:background-size="border" draw:fill="solid" draw:fill-color="#ffffff"/></style:style>'
              '</office:automatic-styles><office:master-styles>'
              '<style:master-page style:name="Default" style:page-layout-name="PM1" draw:style-name="Mdp1">'
              '<draw:frame presentation:style-name="Default-title" draw:layer="backgroundobjects" '
              'svg:width="30.162cm" svg:height="4.286cm" svg:x="1.852cm" svg:y="1.014cm" presentation:class="title" '
              'presentation:placeholder="true"><draw:text-box/></draw:frame>'
              '<draw:frame presentation:style-name="Default-outline1" draw:layer="backgroundobjects" '
              'svg:width="30.162cm" svg:height="11.786cm" svg:x="1.852cm" svg:y="5.072cm" '
              'presentation:class="outline" presentation:placeholder="true"><draw:text-box/></draw:frame>'
              '</style:master-page></office:master-styles></office:document-styles>'
              % (NS, FONT_DECLS, text_props(BODY, "18pt"),
                 pstyle("title", HEAD, "44pt", '<style:graphic-properties draw:fill="none" draw:stroke="none" '
                                               'draw:textarea-vertical-align="middle"/>'),
                 pstyle("subtitle", BODY, "24pt", '<style:graphic-properties draw:fill="none" draw:stroke="none"/>'
                                                    '<style:paragraph-properties fo:text-align="center"/>'), outline))
    content = ('<?xml version="1.0" encoding="UTF-8"?><office:document-content %s>%s<office:body>'
               '<office:presentation><draw:page draw:name="page1" draw:master-page-name="Default" '
               'presentation:presentation-page-layout-name="AL1T0">'
               # the first slide is a title slide, as Office's: a title and a subtitle to click into
               '<draw:frame presentation:style-name="Default-title" draw:layer="layout" svg:width="25.4cm" '
               'svg:height="6.63cm" svg:x="4.233cm" svg:y="3.118cm" presentation:class="title" '
               'presentation:placeholder="true"><draw:text-box/></draw:frame>'
               '<draw:frame presentation:style-name="Default-subtitle" draw:layer="layout" svg:width="25.4cm" '
               'svg:height="4.6cm" svg:x="4.233cm" svg:y="10.001cm" presentation:class="subtitle" '
               'presentation:placeholder="true"><draw:text-box/></draw:frame>'
               '</draw:page></office:presentation></office:body>'
               '</office:document-content>' % (NS, FONT_DECLS))
    return content, styles


def main(out):
    os.makedirs(out, exist_ok=True)
    c, s = writer()
    write(os.path.join(out, "normal.ott"), "application/vnd.oasis.opendocument.text-template", c, s, "Normal")
    c, s = calc()
    write(os.path.join(out, "book.ots"), "application/vnd.oasis.opendocument.spreadsheet-template", c, s, "Book")
    c, s = impress()
    write(os.path.join(out, "blank.otp"), "application/vnd.oasis.opendocument.presentation-template", c, s,
          "Blank presentation")


if __name__ == "__main__":
    main(sys.argv[1])
