#!/usr/bin/python3 -I
# codesign-roots.py MSFT.CSV OUT.pem (defaults/codesign-roots.pem): Microsoft's root program roots for code signing / time stamping (Included or
# NotBefore) that Debian's store lacks, from crt.sh, each checked against CCADB's SHA-256, with its uses
import csv, sys, glob, hashlib, subprocess, urllib.request, time, base64, re
host = set()
for f in glob.glob('/etc/ssl/certs/*.pem'):
    d = subprocess.run(['openssl', 'x509', '-in', f, '-outform', 'DER'], capture_output=True).stdout
    if d: host.add(hashlib.sha256(d).hexdigest().upper())
WANT = {'Code Signing': '1.3.6.1.5.5.7.3.3', 'Time Stamping': '1.3.6.1.5.5.7.3.8'}
out, n, skipped = [], 0, 0
for r in csv.DictReader(open(sys.argv[1], encoding='utf-8')):
    if r['Microsoft Status'] not in ('Included', 'NotBefore'): continue
    ekus = [e.strip() for e in r['Microsoft EKUs'].split(';')]
    uses = [WANT[e] for e in ekus if e in WANT]
    if not uses: continue
    sha = r['SHA-256 Fingerprint'].upper()
    if sha in host: skipped += 1; continue
    for attempt in range(4):
        try:
            pem = urllib.request.urlopen(urllib.request.Request('https://crt.sh/?d=' + sha, headers={'User-Agent': 'sg-roots'}), timeout=60).read().decode()
            break
        except Exception as ex:
            time.sleep(5 * (attempt + 1)); pem = ''
    m = re.search(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', pem, re.S)
    der = base64.b64decode(''.join(m.group(1).split())) if m else b''
    if hashlib.sha256(der).hexdigest().upper() != sha:
        print('MISMATCH/MISSING', r['CA Common Name or Certificate Name'], file=sys.stderr); continue
    name = r['CA Common Name or Certificate Name'].replace('\n', ' ')
    out.append('# %s (%s; Microsoft: %s)\n# uses: %s\n%s' % (name, r['CA Owner'], r['Microsoft Status'], ' '.join(uses), pem[m.start():m.end()] + '\n'))
    n += 1
    time.sleep(1.5)
open(sys.argv[2], 'w').write(''.join(out))
print('roots', n, 'already in the host store', skipped)
