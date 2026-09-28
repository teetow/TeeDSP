"""Own only the selected stream links. Restore direct playback on shutdown/failure."""
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import re
import volume_bridge

SOURCE = os.environ.get("TEEDSP_SOURCE", "shairport-sync")
SINK = os.environ.get("TEEDSP_SINK", "alsa_output.usb-Roland_EDIROL_UA-25-00.analog-stereo")


def graph():
    # Bookworm's pw-dump emits invalid JSON for some newer server parameters.
    # pw-link's stable port names avoid both that version mismatch and volatile IDs.
    ports, links = {}, set()
    for direction in ("-i", "-o"):
        output = subprocess.check_output(["pw-link", direction], timeout=5, text=True)
        for line in output.splitlines():
            name = line.strip()
            if ":" in name:
                ports[tuple(name.split(":", 1))] = name
    output = subprocess.check_output(["pw-link", "-l"], timeout=5, text=True)
    current = None
    for line in output.splitlines():
        name = line.strip()
        if name.startswith("|->") and current:
            links.add((current, name[3:].strip()))
        elif name.startswith("|<-") and current:
            links.add((name[3:].strip(), current))
        elif ":" in name:
            current = name
    return ports, links


def plan(ports, links, restore=False):
    """Return additions/deletions only for our source, filter and physical sink."""
    add, remove = [], []
    for ch in ("FL", "FR"):
        source = ports.get((SOURCE, "output_" + ch))
        sink = ports.get((SINK, "playback_" + ch))
        effect_in = ports.get(("teedsp", "input_" + ch))
        effect_out = ports.get(("teedsp", "output_" + ch))
        if source is None or sink is None:
            continue
        direct = (source, sink)
        if restore:
            # Only restore streams that we actually routed, or whose effect has died.
            if direct not in links:
                add.append(direct)
            if effect_in is not None and (source, effect_in) in links:
                remove.append((source, effect_in))
        elif effect_in is not None and effect_out is not None:
            for link in ((effect_out, sink), (source, effect_in)):
                if link not in links:
                    add.append(link)
            if direct in links:
                remove.append(direct)
    return add, remove


def apply(ports, links, restore=False):
    add, remove = plan(ports, links, restore)
    if restore and (add or remove):
        # Native AirPlay is full-level now. Preserve the listening volume when
        # handing back to the direct path during an orderly stop/child failure.
        try:
            volume = json.loads(Path("/data/volume.json").read_text())
            gain = 0 if volume.get("muted") else (volume["volume"] / 100) ** 1.660964
            source_gain(gain)
        except FileNotFoundError:
            pass
    # Connect before removing the old path: never leave the source disconnected
    # if PipeWire rejects the new link. A switch can have a brief overlap.
    for source, dest in add:
        subprocess.run(["pw-link", str(source), str(dest)], timeout=3, check=True, capture_output=True)
    for source, dest in remove:
        subprocess.run(["pw-link", "-d", str(source), str(dest)], timeout=3, check=True, capture_output=True)
    if not restore and (add or remove):
        source_gain(1)
    return bool(add or remove)


def source_gain(gain):
    output = subprocess.check_output(["pw-cli", "ls", "Node"], text=True, timeout=5)
    for block in re.split(r"(?=\s*id \d+, type)", output):
        match = re.search(r"id (\d+),", block)
        name = re.search(r'node.name = "([^"]+)"', block)
        if match and name and name[1] == SOURCE:
            props = json.dumps({"channelVolumes": [gain, gain]})
            subprocess.run(["pw-cli", "set-param", match[1], "Props", props], check=True, timeout=3, capture_output=True)
            return


def status(value):
    path = Path("/tmp/teedsp-route.json")
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(value))
    temp.replace(path)


def main():
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    child = subprocess.Popen(["/usr/local/bin/teedsp"])
    volume_bridge.start()
    try:
        while not stopping and child.poll() is None:
            try:
                ports, links = graph()
                if apply(ports, links):
                    print("Routed", SOURCE, "through TeeDSP to", SINK, flush=True)
                ports, links = graph()
                routed = all(
                    (ports.get((SOURCE, "output_" + c)), ports.get(("teedsp", "input_" + c))) in links
                    and (ports.get(("teedsp", "output_" + c)), ports.get((SINK, "playback_" + c))) in links
                    for c in ("FL", "FR"))
                status({"source": SOURCE, "sink": SINK, "routed": routed})
            except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
                print("Routing retry:", error, flush=True)
                status({"routed": False, "error": str(error)})
            time.sleep(1)
    finally:
        try:
            apply(*graph(), restore=True)
            print("Restored direct AirPlay playback", flush=True)
        except Exception as error:
            print("Could not restore routing:", error, flush=True)
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
    return 0 if stopping else 1


if __name__ == "__main__":
    raise SystemExit(main())
