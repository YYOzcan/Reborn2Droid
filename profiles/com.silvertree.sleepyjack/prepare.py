"""Sleepy Jack: assets/sleepyjack_assets/* are AES-256-CBC encrypted with a key made from the CRCs of
classes.dex, resources.arsc and the managed DLLs inside the original APK, then decrypted byte by byte in
C# at startup (and again after 60 s as a crack check). A repackaged APK changes those CRCs, so the assets
are stored pre-decrypted here and ilpatch.txt turns the C# decryption into a plain read."""
import struct, subprocess

KEY_FILES = ["classes.dex", "assets/bin/Data/Managed/Assembly-CSharp-firstpass.dll", "assets/bin/Data/Managed/Assembly-CSharp.dll",
             "assets/bin/Data/Managed/UnityEngine.dll", "assets/bin/Data/Managed/mscorlib.dll",
             "assets/bin/Data/Managed/ICSharpCode.SharpZipLib.dll", "classes.dex", "assets/bin/Data/Managed/System.dll",
             "resources.arsc", "res/layout/main.xml"]

def transform(z):
    crcs = [z.getinfo(f).CRC for f in KEY_FILES]
    if "encryptFlag" not in z.namelist(): crcs[0] += 3
    key = b"".join(struct.pack("<q", c)[:4] for c in crcs[:8])
    iv = b"".join(struct.pack("<q", c)[:4] for c in crcs[6:10])
    out = {}
    for name in z.namelist():
        if not name.startswith("assets/sleepyjack_assets/") or name.endswith("/"): continue
        d = z.read(name); size = struct.unpack("<i", d[:4])[0]
        p = subprocess.run(["openssl", "enc", "-d", "-aes-256-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                           input=d[4:], capture_output=True, check=True)
        out[name] = d[:4] + p.stdout[:size]          # same layout: [size][data], now plain
        print(f"   decrypted {name} ({size} bytes)")
    return out
