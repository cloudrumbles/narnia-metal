#!/usr/bin/env python3
"""Write a SideStore/AltStore source (source.json) for the latest IPA.

Add the source URL in SideStore once; every new build then shows up under
Updates (SideStore checks sources when it refreshes apps).

usage: make-source.py <version> <build> <ipa path> <download url> <out json>
"""
import datetime
import json
import os
import sys

version, build, ipa, url, out = sys.argv[1:6]
source = {
    "name": "Narnia Metal",
    "identifier": "tech.cloudrumbles.narniametal.source",
    "subtitle": "Vulkan streaming from a narnia box",
    "website": "https://github.com/cloudrumbles/narnia-metal",
    "apps": [{
        "name": "Narnia Metal",
        "bundleIdentifier": "tech.cloudrumbles.narniametal",
        "developerName": "cloudrumbles",
        "subtitle": "Runs a box's Vulkan on this iPad",
        "localizedDescription": "Host side of narnia-metal: connects out to nm-relay on a box and runs its Vulkan streams on vkr + MoltenVK.",
        "iconURL": "https://raw.githubusercontent.com/cloudrumbles/narnia-metal/main/ios/app/icon.png",
        "tintColor": "#3a7bd5",
        "versions": [{
            "version": version,
            "buildVersion": build,
            "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "localizedDescription": os.environ.get("NOTES", "Build " + build),
            "downloadURL": url,
            "size": os.path.getsize(ipa),
            "minOSVersion": "16.0",
        }],
        "appPermissions": {"entitlements": [], "privacy": {}},
    }],
    "news": [],
}
with open(out, "w") as f:
    json.dump(source, f, indent=2)
