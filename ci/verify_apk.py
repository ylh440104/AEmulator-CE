import hashlib
from pathlib import Path
import sys
import zipfile

apk = Path(sys.argv[1])
result = []
with zipfile.ZipFile(apk) as z:
    for name in ('libqemu_senses.so', 'libsensesbinderd.so'):
        expected = Path('app/src/main/jniLibs/arm64-v8a') / name
        data = z.read('lib/arm64-v8a/' + name)
        digest = hashlib.sha256(data).hexdigest()
        if digest != hashlib.sha256(expected.read_bytes()).hexdigest():
            raise SystemExit('APK contains an outdated engine: ' + name)
        result.append(digest + '  ' + name)
Path('out/apk-engine-sha256.txt').write_text('\n'.join(result) + '\n')
print('\n'.join(result))