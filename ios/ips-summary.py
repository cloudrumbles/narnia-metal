#!/usr/bin/env python3
"""Print the crashing thread of macOS .ips crash reports given as arguments."""
import json, sys
for path in sys.argv[1:]:
    text = open(path).read()
    head, _, body = text.partition("\n")
    try:
        r = json.loads(body)
    except Exception:
        print(text[:4000]); continue
    print("==", path, r.get("exception"), r.get("termination"))
    imgs = r.get("usedImages", [])
    t = r["threads"][r.get("faultingThread", 0)]
    for i, f in enumerate(t["frames"][:40]):
        img = imgs[f["imageIndex"]].get("name", "?") if "imageIndex" in f else "?"
        print(f"{i:2} {img} {f.get('symbol', '?')} +{f.get('symbolLocation', '?')}")
