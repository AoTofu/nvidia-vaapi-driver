#!/usr/bin/env python3
"""Observe an existing Chrome session without copying its profile or enabling CDP.

Start Chrome with NVD_LOG and NVD_LOG_VERBOSE=1, set the desired playback speed
in its UI, then pass its org.mpris.MediaPlayer2.chromium.instance... service.
This records playback progress and driver activity; it cannot identify the
displayed codec from MPRIS alone. AV1 log counts are scoped to the supplied log,
which should belong to a browser playing only the video under investigation.
"""
import argparse
import json
from pathlib import Path
import subprocess
import statistics
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("service")
parser.add_argument("driver_log", type=Path)
parser.add_argument("output_dir", type=Path)
parser.add_argument("--seconds", type=float, default=480)
parser.add_argument("--rate", type=float, help="Expected rate while Playing")
parser.add_argument("--until", type=float, help="Stop at this video position in seconds")
args = parser.parse_args()
if not 1 <= args.seconds <= 3600:
    parser.error("--seconds must be between 1 and 3600")
if args.rate is not None and not 0 < args.rate <= 16:
    parser.error("--rate must be positive and at most 16")
if args.until is not None and not 0 <= args.until < float("inf"):
    parser.error("--until must be finite and nonnegative")
args.output_dir.mkdir(parents=True, exist_ok=False)
offset = args.driver_log.stat().st_size
records = []
start = time.monotonic()


def get_property(name):
    result = subprocess.run(
        ["busctl", "--user", "get-property", args.service,
         "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", name],
        check=True, capture_output=True, text=True, timeout=5,
    )
    return json.loads(result.stdout.strip().split(" ", 1)[1])


while time.monotonic() - start < args.seconds:
    sample = {"elapsed": time.monotonic() - start}
    try:
        sample.update({name: get_property(name) for name in
                       ("Position", "Rate", "PlaybackStatus")})
    except (subprocess.SubprocessError, ValueError) as error:
        # A page reload can temporarily remove the media session.
        sample["queryError"] = str(error)
    with args.driver_log.open("rb") as stream:
        if args.driver_log.stat().st_size < offset:
            offset = 0
            sample["logTruncated"] = True
        stream.seek(offset)
        chunk = stream.read().decode(errors="replace")
        offset = stream.tell()
    sample["av1Pictures"] = chunk.count("copyAV1PicParam AV1 color metadata")
    sample["nativeReimports"] = chunk.count("Reimported native")
    sample["driverErrors"] = [line for line in chunk.splitlines() if any(
        term in line for term in ("CUDA_ERROR", "Unable to import external surface",
                                  " failed", "Failed", "ERROR"))]
    records.append(sample)
    with (args.output_dir / "samples.jsonl").open("a") as output:
        output.write(json.dumps(sample) + "\n")
    if len(records) % 30 == 1:
        print(json.dumps(sample), flush=True)
    if args.until is not None and sample.get("Position", 0) / 1e6 >= args.until:
        break
    time.sleep(1)

playing = [sample for sample in records if sample.get("PlaybackStatus") == "Playing"]
ratios = [(end["Position"] - begin["Position"]) / 1e6 / (end["elapsed"] - begin["elapsed"])
          for begin, end in zip(playing, playing[1:])]
summary = {
    "service": args.service, "driverLog": str(args.driver_log.resolve()),
    "first": records[0], "last": records[-1], "samples": len(records),
    "playingRates": sorted({sample["Rate"] for sample in playing}),
    "medianProgressRatio": statistics.median(ratios) if ratios else None,
    "maxPositionSeconds": max(sample.get("Position", 0) for sample in records) / 1e6,
    "av1Pictures": sum(sample["av1Pictures"] for sample in records),
    "nativeReimports": sum(sample["nativeReimports"] for sample in records),
    "driverErrors": [line for sample in records for line in sample["driverErrors"]],
}
(args.output_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary), flush=True)
if args.rate is not None and (not playing or any(sample["Rate"] != args.rate for sample in playing)):
    raise SystemExit("Expected playback rate was not maintained")
if args.until is not None and summary["maxPositionSeconds"] < args.until:
    raise SystemExit("Playback did not reach the requested position")
