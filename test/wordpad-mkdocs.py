#!/usr/bin/python3
# Test documents for test/wordpad-check.sh, made here from the formats'
# specifications (ECMA-376, OASIS ODF) -- nothing third-party is committed.
#
#   wordpad-mkdocs.py OUTDIR
#
# Writes in.docx and in.odt (the same content: a bold heading, a paragraph
# with italic, underline, red and highlighted words, a centred paragraph, a
# two-item bulleted list, a numbered item, a picture 2 x 1 inches of red over
# blue halves, a 2 x 2 table 2000 + 3000 twips wide) and pic.png.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import io
import sys
import zipfile

from PIL import Image

out = sys.argv[1]
img = Image.new("RGB", (192, 96), (255, 0, 0))
for x in range(192):
    for y in range(48, 96):
        img.putpixel((x, y), (0, 0, 255))
buf = io.BytesIO()
img.save(buf, "PNG")
png = buf.getvalue()
open(out + "/pic.png", "wb").write(png)

W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
doc = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document {W} xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
 xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
 xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
 xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Quarterly Report</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Plain then </w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>italic</w:t></w:r>
<w:r><w:t xml:space="preserve"> and </w:t></w:r><w:r><w:rPr><w:u w:val="single"/></w:rPr><w:t>underlined</w:t></w:r>
<w:r><w:t xml:space="preserve"> and </w:t></w:r><w:r><w:rPr><w:color w:val="FF0000"/></w:rPr><w:t>red</w:t></w:r>
<w:r><w:t xml:space="preserve"> and </w:t></w:r><w:r><w:rPr><w:highlight w:val="yellow"/></w:rPr><w:t>marked</w:t></w:r>
<w:r><w:t xml:space="preserve"> caf&#233; </w:t></w:r><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:t>2</w:t></w:r></w:p>
<w:p><w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:rPr><w:sz w:val="36"/><w:rFonts w:ascii="Courier New" w:hAnsi="Courier New"/></w:rPr><w:t>Centred big</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>First bullet</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>Second bullet</w:t></w:r></w:p>
<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="2"/></w:numPr></w:pPr><w:r><w:t>Numbered item</w:t></w:r></w:p>
<w:p><w:r><w:drawing><wp:inline><wp:extent cx="1828800" cy="914400"/><wp:docPr id="1" name="p"/>
<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic>
<pic:blipFill><a:blip r:embed="rId9"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>
<w:tbl><w:tblPr><w:tblW w:w="0" w:type="auto"/></w:tblPr><w:tblGrid><w:gridCol w:w="2000"/><w:gridCol w:w="3000"/></w:tblGrid>
<w:tr><w:tc><w:tcPr><w:tcW w:w="2000" w:type="dxa"/></w:tcPr><w:p><w:r><w:t>Cell A1</w:t></w:r></w:p></w:tc>
<w:tc><w:tcPr><w:tcW w:w="3000" w:type="dxa"/></w:tcPr><w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Cell B1</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:p><w:r><w:t>Cell A2</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Cell B2</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:p><w:r><w:t>The end.</w:t></w:r></w:p>
</w:body></w:document>'''
styles = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles {W}><w:docDefaults><w:rPrDefault><w:rPr><w:sz w:val="24"/></w:rPr></w:rPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:styleId="Normal"><w:name w:val="Normal"/></w:style>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/>
<w:rPr><w:b/><w:sz w:val="32"/></w:rPr></w:style></w:styles>'''
numbering = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering {W}>
<w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/><w:lvlText w:val="*"/></w:lvl></w:abstractNum>
<w:abstractNum w:abstractNumId="1"><w:lvl w:ilvl="0"><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/></w:lvl></w:abstractNum>
<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num><w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num></w:numbering>'''
rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/image1.png"/>
</Relationships>'''
ct = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/><Default Extension="png" ContentType="image/png"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
</Types>'''
pkgrels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>'''
with zipfile.ZipFile(out + "/in.docx", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", ct)
    z.writestr("_rels/.rels", pkgrels)
    z.writestr("word/document.xml", doc)
    z.writestr("word/styles.xml", styles)
    z.writestr("word/numbering.xml", numbering)
    z.writestr("word/_rels/document.xml.rels", rels)
    z.writestr("word/media/image1.png", png)

NS = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
      'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
      'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
      'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
      'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
      'xmlns:xlink="http://www.w3.org/1999/xlink" '
      'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
      'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0"')
content = f'''<?xml version="1.0" encoding="UTF-8"?>
<office:document-content {NS} office:version="1.3"><office:automatic-styles>
<style:style style:name="H" style:family="paragraph"><style:text-properties fo:font-weight="bold" fo:font-size="16pt"/></style:style>
<style:style style:name="C" style:family="paragraph"><style:paragraph-properties fo:text-align="center"/></style:style>
<style:style style:name="I" style:family="text"><style:text-properties fo:font-style="italic"/></style:style>
<style:style style:name="U" style:family="text"><style:text-properties style:text-underline-style="solid"/></style:style>
<style:style style:name="R" style:family="text"><style:text-properties fo:color="#ff0000"/></style:style>
<style:style style:name="M" style:family="text"><style:text-properties fo:background-color="#ffff00"/></style:style>
<style:style style:name="S" style:family="text"><style:text-properties style:text-position="super 58%"/></style:style>
<style:style style:name="B" style:family="text"><style:text-properties fo:font-size="18pt" style:font-name="Courier New"/></style:style>
<style:style style:name="TC1" style:family="table-column"><style:table-column-properties style:column-width="1.3889in"/></style:style>
<style:style style:name="TC2" style:family="table-column"><style:table-column-properties style:column-width="2.0833in"/></style:style>
<text:list-style style:name="LB"><text:list-level-style-bullet text:level="1" text:bullet-char="*"/></text:list-style>
<text:list-style style:name="LN"><text:list-level-style-number text:level="1" style:num-format="1"/></text:list-style>
</office:automatic-styles><office:body><office:text>
<text:p text:style-name="H">Quarterly Report</text:p>
<text:p>Plain then <text:span text:style-name="I">italic</text:span> and <text:span text:style-name="U">underlined</text:span> and <text:span text:style-name="R">red</text:span> and <text:span text:style-name="M">marked</text:span> caf&#233; <text:span text:style-name="S">2</text:span></text:p>
<text:p text:style-name="C"><text:span text:style-name="B">Centred big</text:span></text:p>
<text:list text:style-name="LB"><text:list-item><text:p>First bullet</text:p></text:list-item><text:list-item><text:p>Second bullet</text:p></text:list-item></text:list>
<text:list text:style-name="LN"><text:list-item><text:p>Numbered item</text:p></text:list-item></text:list>
<text:p><draw:frame svg:width="2in" svg:height="1in" text:anchor-type="as-char"><draw:image xlink:href="Pictures/p.png"/></draw:frame></text:p>
<table:table table:name="T1"><table:table-column table:style-name="TC1"/><table:table-column table:style-name="TC2"/>
<table:table-row><table:table-cell><text:p>Cell A1</text:p></table:table-cell><table:table-cell><text:p><text:span text:style-name="I">Cell B1</text:span></text:p></table:table-cell></table:table-row>
<table:table-row><table:table-cell><text:p>Cell A2</text:p></table:table-cell><table:table-cell><text:p>Cell B2</text:p></table:table-cell></table:table-row></table:table>
<text:p>The end.</text:p>
</office:text></office:body></office:document-content>'''
manifest = '''<?xml version="1.0" encoding="UTF-8"?>
<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0">
<manifest:file-entry manifest:full-path="/" manifest:media-type="application/vnd.oasis.opendocument.text"/>
<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>
<manifest:file-entry manifest:full-path="Pictures/p.png" manifest:media-type="image/png"/>
</manifest:manifest>'''
with zipfile.ZipFile(out + "/in.odt", "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/vnd.oasis.opendocument.text", zipfile.ZIP_STORED)
    z.writestr("META-INF/manifest.xml", manifest, zipfile.ZIP_DEFLATED)
    z.writestr("content.xml", content, zipfile.ZIP_DEFLATED)
    z.writestr("Pictures/p.png", png, zipfile.ZIP_STORED)
