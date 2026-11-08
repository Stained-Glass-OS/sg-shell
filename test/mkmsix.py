#!/usr/bin/env python3
# Write an unsigned AppX/MSIX package or bundle from a description -- the
# fixtures for wine-sg's test/msix-gate.sh (framework packages, dependencies,
# resource packages, app execution aliases, winget), copied here for SG Store's
# gate (store-check G4). No Microsoft tool is needed
# (makeappx is one); the gate signs the result with osslsigncode and a
# certificate it generates for the run.
#
#   mkmsix.py package OUT --name N --version V [--arch x64] [--publisher P]
#             [--framework] [--display-name D] [--resource-id R]
#             [--language L ...] [--dep NAME:MINVERSION ...]
#             [--app ID:EXE[:ALIAS,ALIAS...][:DISPLAYNAME] ...]
#             [--file SRC:NAME_IN_PACKAGE ...] [--allow-external] [--extensions FILE]
#             (--allow-external: a package with external location, a "sparse"
#             package: uap10:AllowExternalContent, the applications win32 ones;
#             --extensions: the first application's <Extensions> are FILE's XML,
#             with the uap, uap3, uap5, com, desktop, desktop4, desktop5 and desktop10
#             prefixes declared)
#             [--package-extensions FILE]: the package's own <Extensions> (after
#             <Applications>; a framework's activatable classes) are FILE's XML
#   mkmsix.py bundle OUT --name N --version V [--publisher P] [--unmapped-payload]
#             --pkg FILE:application:ARCH ... --pkg FILE:resource:LANGUAGE ...
#             (--unmapped-payload: the block map leaves the packages out, as
#             newer bundles do; the packages must then be signed themselves)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
import argparse
import base64
import hashlib
import io
import os
import re
import sys
import zipfile
from xml.sax.saxutils import quoteattr, escape

PUBLISHER = 'CN=Stained Glass Test Publisher'
DATE = (2026, 1, 1, 0, 0, 0)

CONTENT_TYPES = (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                 b'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                 b'<Default Extension="exe" ContentType="application/x-msdownload"/>'
                 b'<Default Extension="dll" ContentType="application/x-msdownload"/>'
                 b'<Default Extension="bin" ContentType="application/octet-stream"/>'
                 b'<Default Extension="txt" ContentType="text/plain"/>'
                 b'<Default Extension="png" ContentType="image/png"/>'
                 b'<Default Extension="xml" ContentType="application/vnd.ms-appx.manifest+xml"/>'
                 b'<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>'
                 b'<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>'
                 b'</Types>')
BUNDLE_TYPES = (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                b'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                b'<Default Extension="msix" ContentType="application/vnd.ms-appx"/>'
                b'<Default Extension="xml" ContentType="application/vnd.ms-appx.bundlemanifest+xml"/>'
                b'<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>'
                b'<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>'
                b'</Types>')
LOGO = b'\x89PNG\r\n\x1a\n' + bytes(range(64))


def blockmap(files):
    parts = ['<?xml version="1.0" encoding="UTF-8" standalone="no"?>'
             '<BlockMap xmlns="http://schemas.microsoft.com/appx/2010/blockmap" '
             'HashMethod="http://www.w3.org/2001/04/xmlenc#sha256">']
    for name, data, _ in files:
        lfh = 30 + len(name.replace('\\', '/').encode())
        parts.append(f'<File Name={quoteattr(name)} Size="{len(data)}" LfhSize="{lfh}">')
        for i in range(0, len(data), 65536):
            h = base64.b64encode(hashlib.sha256(data[i:i + 65536]).digest()).decode()
            parts.append(f'<Block Hash="{h}"/>')
        parts.append('</File>')
    parts.append('</BlockMap>')
    return ''.join(parts).encode()


def write_zip(out, files, types, unmapped=()):
    bm = blockmap([f for f in files if f[0] not in unmapped])
    with zipfile.ZipFile(out, 'w') as z:
        for name, content, method in files:
            z.writestr(zipfile.ZipInfo(name.replace('\\', '/'), DATE), content, compress_type=method)
        z.writestr(zipfile.ZipInfo('AppxBlockMap.xml', DATE), bm, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr(zipfile.ZipInfo('[Content_Types].xml', DATE), types, compress_type=zipfile.ZIP_DEFLATED)


def manifest(a):
    rid = f' ResourceId={quoteattr(a.resource_id)}' if a.resource_id else ''
    arch = f' ProcessorArchitecture="{a.arch}"'
    props = [f'<DisplayName>{escape(a.display_name or a.name)}</DisplayName>',
             '<PublisherDisplayName>Stained Glass OS</PublisherDisplayName>',
             '<Logo>Assets\\logo.png</Logo>']
    if a.framework:
        props.append('<Framework>true</Framework>')
    if a.resource_id and a.language:
        props.append('<ResourcePackage>true</ResourcePackage>')
    if a.allow_external:
        props.append('<uap10:AllowExternalContent>true</uap10:AllowExternalContent>')
    deps = ['<TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.22621.0"/>']
    for d in a.dep:
        name, minv = d.split(':')
        deps.append(f'<PackageDependency Name={quoteattr(name)} MinVersion="{minv}" Publisher={quoteattr(a.publisher)}/>')
    res = ''.join(f'<Resource Language="{lang}"/>' for lang in (a.language or ['en-us']))
    apps = []
    for spec in a.app:
        f = spec.split(':')
        app_id, exe = f[0], f[1]
        aliases = [x for x in f[2].split(',') if x] if len(f) > 2 else []
        disp = f[3] if len(f) > 3 else f'{app_id} app'
        ext = ''
        if a.extensions and not apps:
            with open(a.extensions) as f:
                ext = '<Extensions>' + f.read() + '</Extensions>'
        elif aliases:
            ext = ('<Extensions><uap5:Extension Category="windows.appExecutionAlias">'
                   '<uap5:AppExecutionAlias>' +
                   ''.join(f'<uap5:ExecutionAlias Alias={quoteattr(al)}/>' for al in aliases) +
                   '</uap5:AppExecutionAlias></uap5:Extension></Extensions>')
        sparse = ' uap10:TrustLevel="mediumIL" uap10:RuntimeBehavior="win32App"' if a.allow_external else ''
        apps.append(f'<Application Id={quoteattr(app_id)} Executable={quoteattr(exe)} '
                    f'EntryPoint="Windows.FullTrustApplication"{sparse}>'
                    f'<uap:VisualElements DisplayName={quoteattr(disp)} Description="test" '
                    f'BackgroundColor="transparent" Square150x150Logo="Assets\\logo.png" '
                    f'Square44x44Logo="Assets\\logo.png"/>{ext}</Application>')
    body = (f'<?xml version="1.0" encoding="utf-8"?>\n'
            f'<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10" '
            f'xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10" '
            f'xmlns:uap3="http://schemas.microsoft.com/appx/manifest/uap/windows10/3" '
            f'xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5" '
            f'xmlns:uap10="http://schemas.microsoft.com/appx/manifest/uap/windows10/10" '
            f'xmlns:com="http://schemas.microsoft.com/appx/manifest/com/windows10" '
            f'xmlns:desktop="http://schemas.microsoft.com/appx/manifest/desktop/windows10" '
            f'xmlns:desktop4="http://schemas.microsoft.com/appx/manifest/desktop/windows10/4" '
            f'xmlns:desktop5="http://schemas.microsoft.com/appx/manifest/desktop/windows10/5" '
            f'xmlns:desktop10="http://schemas.microsoft.com/appx/manifest/desktop/windows10/10" '
            f'xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities" '
            f'IgnorableNamespaces="uap uap5 uap10 rescap">'
            f'<Identity Name={quoteattr(a.name)}{arch} Publisher={quoteattr(a.publisher)} '
            f'Version="{a.version}"{rid}/>'
            f'<Properties>{"".join(props)}</Properties>'
            f'<Dependencies>{"".join(deps)}</Dependencies>'
            f'<Resources>{res}</Resources>')
    if not a.framework and not (a.resource_id and a.language):
        body += '<Capabilities><rescap:Capability Name="runFullTrust"/></Capabilities>'
    if apps:
        body += f'<Applications>{"".join(apps)}</Applications>'
    if getattr(a, 'package_extensions', None):
        with open(a.package_extensions) as f:
            body += '<Extensions>' + f.read() + '</Extensions>'
    body += '</Package>\n'
    return body.encode()


def cmd_package(a):
    files = []
    for spec in a.file:
        src, dst = spec.split(':', 1)
        with open(src, 'rb') as f:
            files.append((dst, f.read(), zipfile.ZIP_DEFLATED))
    files.append(('Assets\\logo.png', LOGO, zipfile.ZIP_STORED))
    files.append(('AppxManifest.xml', manifest(a), zipfile.ZIP_DEFLATED))
    write_zip(a.out, files, CONTENT_TYPES)


def cmd_bundle(a):
    files, entries = [], []
    for spec in a.pkg:
        path, kind, what = spec.split(':')
        with open(path, 'rb') as f:
            data = f.read()
        name = os.path.basename(path)
        files.append((name, data, zipfile.ZIP_STORED))
        inner = zipfile.ZipFile(io.BytesIO(data)).read('AppxManifest.xml').decode()
        version = re.search(r'<Identity[^>]*Version="([^"]+)"', inner).group(1)
        if kind == 'application':
            entries.append(f'<Package Type="application" Version="{version}" Architecture="{what}" '
                           f'FileName="{name}" Offset="0" Size="{len(data)}"/>')
        else:
            rid = re.search(r'<Identity[^>]*ResourceId="([^"]+)"', inner).group(1)
            entries.append(f'<Package Type="resource" Version="{version}" ResourceId="{rid}" FileName="{name}" '
                           f'Offset="0" Size="{len(data)}"><Resources><Resource Language="{what}"/>'
                           f'</Resources></Package>')
    bman = (f'<?xml version="1.0" encoding="utf-8"?>'
            f'<Bundle xmlns="http://schemas.microsoft.com/appx/2013/bundle" SchemaVersion="5.0">'
            f'<Identity Name={quoteattr(a.name)} Publisher={quoteattr(a.publisher)} Version="{a.version}"/>'
            f'<Packages>{"".join(entries)}</Packages></Bundle>').encode()
    files.append(('AppxMetadata\\AppxBundleManifest.xml', bman, zipfile.ZIP_DEFLATED))
    # newer bundles map only their metadata; each package is signed on its own
    unmapped = [f[0] for f in files if f[0].endswith('.msix')] if a.unmapped_payload else ()
    write_zip(a.out, files, BUNDLE_TYPES, unmapped)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('kind', choices=('package', 'bundle'))
    p.add_argument('out')
    p.add_argument('--name', required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--arch', default='x64')
    p.add_argument('--publisher', default=PUBLISHER)
    p.add_argument('--framework', action='store_true')
    p.add_argument('--display-name')
    p.add_argument('--resource-id')
    p.add_argument('--language', action='append', default=[])
    p.add_argument('--dep', action='append', default=[])
    p.add_argument('--app', action='append', default=[])
    p.add_argument('--file', action='append', default=[])
    p.add_argument('--pkg', action='append', default=[])
    p.add_argument('--unmapped-payload', action='store_true')
    p.add_argument('--allow-external', action='store_true')
    p.add_argument('--extensions')
    p.add_argument('--package-extensions')   # the package's own <Extensions> (activatable classes)
    a = p.parse_args()
    (cmd_package if a.kind == 'package' else cmd_bundle)(a)


if __name__ == '__main__':
    sys.exit(main())
