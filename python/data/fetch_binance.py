#!/usr/bin/env python3
"""Download and verify Binance public historical trade data.

Source: https://data.binance.vision/ -- Binance's own archive of aggregated
trades, published as monthly and daily ZIPs with SHA256 checksums. No API key,
no rate limit, and the same bytes for everyone, which is what makes an
experiment built on it reproducible by a reader.

Every download is checksum-verified against the published .CHECKSUM file. A
corrupted or truncated archive that silently produced a shorter event stream
would change experiment results in a way nobody would notice, so a mismatch is
a hard failure rather than a warning.

Usage:
    python fetch_binance.py --symbol BTCUSDT --month 2024-01
    python fetch_binance.py --symbol ETHUSDT --date 2024-01-15 --out data/raw
    python fetch_binance.py --symbol BTCUSDT --dates 2024-01-08:2024-02-04

A walk-forward evaluation needs a contiguous run of days, not one. `--dates`
downloads a closed interval, skipping days already present, and stops on the
first day the archive does not exist rather than leaving a hole in the middle
of the series -- a gap would silently turn an expanding window into a
discontinuous one.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

BASE = "https://data.binance.vision/data/spot"

# Column layout of an aggTrades CSV. Binance ships these files without a
# header row, so the order is load-bearing and recorded here rather than
# guessed at the call site.
AGG_TRADE_COLUMNS = [
    "agg_trade_id",
    "price",
    "quantity",
    "first_trade_id",
    "last_trade_id",
    "transact_time",   # milliseconds since epoch
    "is_buyer_maker",  # true => the buyer was the passive side, so the SELLER
                       #         was the aggressor
    "is_best_match",
]


def _url(symbol: str, period: str, kind: str = "aggTrades") -> str:
    """Build the archive URL. `period` is YYYY-MM or YYYY-MM-DD."""
    granularity = "monthly" if len(period) == 7 else "daily"
    name = f"{symbol}-{kind}-{period}.zip"
    return f"{BASE}/{granularity}/{kind}/{symbol}/{name}"


def _download(url: str, dest: Path) -> None:
    print(f"  GET {url}")
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
            total = int(r.headers.get("Content-Length", 0))
            got = 0
            while chunk := r.read(1 << 20):
                f.write(chunk)
                got += len(chunk)
                if total:
                    pct = 100.0 * got / total
                    print(f"\r  {got/1e6:7.1f} / {total/1e6:.1f} MB ({pct:5.1f}%)",
                          end="", flush=True)
            print()
    except urllib.error.HTTPError as e:
        raise SystemExit(
            f"\nHTTP {e.code} for {url}\n"
            "Check the symbol and period. Binance publishes a month only after "
            "it has ended, and daily files only for roughly the last few months."
        ) from e


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def fetch(symbol: str, period: str, out_dir: Path, keep_zip: bool = False) -> Path:
    """Download, verify, and extract one archive. Returns the CSV path."""
    out_dir.mkdir(parents=True, exist_ok=True)
    url = _url(symbol, period)
    zip_path = out_dir / Path(url).name
    csv_path = zip_path.with_suffix(".csv")

    if csv_path.exists():
        print(f"  already present: {csv_path}")
        return csv_path

    _download(url, zip_path)

    # Verify before trusting a single byte of it.
    checksum_path = zip_path.with_suffix(".zip.CHECKSUM")
    _download(url + ".CHECKSUM", checksum_path)
    expected = checksum_path.read_text().split()[0].strip()
    actual = _sha256(zip_path)
    if actual != expected:
        zip_path.unlink(missing_ok=True)
        raise SystemExit(
            f"CHECKSUM MISMATCH for {zip_path.name}\n"
            f"  expected {expected}\n"
            f"  actual   {actual}\n"
            "The archive was corrupted in transit. Refusing to use it: a "
            "truncated file would quietly shorten the event stream and shift "
            "every downstream number."
        )
    print(f"  sha256 verified: {actual[:16]}...")

    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        if len(names) != 1:
            raise SystemExit(f"expected one member in {zip_path.name}, got {names}")
        with z.open(names[0]) as src, open(csv_path, "wb") as dst:
            dst.write(src.read())
    print(f"  extracted: {csv_path} ({csv_path.stat().st_size/1e6:.1f} MB)")

    checksum_path.unlink(missing_ok=True)
    if not keep_zip:
        zip_path.unlink(missing_ok=True)

    _write_manifest(csv_path, symbol, period, url, actual)
    return csv_path


def _write_manifest(csv_path: Path, symbol: str, period: str,
                    url: str, sha: str) -> None:
    """Record provenance beside the data.

    An experiment that cannot say which bytes it consumed is not reproducible,
    so every CSV carries a sibling .manifest naming its source and checksum.
    """
    manifest = csv_path.with_suffix(".manifest")
    lines = 0
    with open(csv_path, "rb") as f:
        for lines, _ in enumerate(f, 1):
            pass
    manifest.write_text(
        f"symbol={symbol}\n"
        f"period={period}\n"
        f"source_url={url}\n"
        f"archive_sha256={sha}\n"
        f"rows={lines}\n"
        f"columns={','.join(AGG_TRADE_COLUMNS)}\n"
    )
    print(f"  manifest:  {manifest} ({lines} rows)")


def _date_range(spec: str) -> list[str]:
    """Expand "YYYY-MM-DD:YYYY-MM-DD" into an inclusive list of days."""
    try:
        lo_s, hi_s = spec.split(":", 1)
        lo = _dt.date.fromisoformat(lo_s)
        hi = _dt.date.fromisoformat(hi_s)
    except ValueError as e:
        raise SystemExit(f"--dates wants YYYY-MM-DD:YYYY-MM-DD, got {spec!r} ({e})")
    if hi < lo:
        raise SystemExit(f"--dates range runs backwards: {lo} to {hi}")
    n = (hi - lo).days + 1
    return [(lo + _dt.timedelta(days=i)).isoformat() for i in range(n)]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbol", default="BTCUSDT")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--month", help="YYYY-MM (monthly archive)")
    g.add_argument("--date", help="YYYY-MM-DD (daily archive)")
    g.add_argument("--dates", metavar="START:END",
                   help="inclusive YYYY-MM-DD:YYYY-MM-DD range of daily archives")
    ap.add_argument("--out", default="data/raw", type=Path)
    ap.add_argument("--keep-zip", action="store_true")
    args = ap.parse_args()

    if args.dates:
        days = _date_range(args.dates)
        print(f"Fetching {args.symbol} aggTrades for {len(days)} days "
              f"({days[0]} .. {days[-1]})")
        paths = []
        for i, day in enumerate(days, 1):
            print(f"\n[{i}/{len(days)}] {day}")
            paths.append(fetch(args.symbol, day, args.out, args.keep_zip))
        total_mb = sum(p.stat().st_size for p in paths) / 1e6
        print(f"\nReady: {len(paths)} files, {total_mb:.0f} MB in {args.out}")
        print("Walk-forward over them with:")
        print(f"  ./build/titans_walkforward {args.out}/{args.symbol}-aggTrades-*.csv")
        return

    period = args.month or args.date
    print(f"Fetching {args.symbol} aggTrades for {period}")
    csv_path = fetch(args.symbol, period, args.out, args.keep_zip)
    print(f"\nReady: {csv_path}")
    print("Label it with:  ./build/titans_replay --label-toxic-flow "
          f"{csv_path}")


if __name__ == "__main__":
    sys.exit(main())
