#!/usr/bin/env python3

import argparse
import glob
import re
import sys
from pathlib import Path

RECORD_PATTERN = re.compile(
    r"period_cmp .*?cco=(?P<cco_mac>\d{12}) "
    r".*?ccodata_num=(?P<cco_count>\d+) "
    r"ccodata=\[(?P<cco_data>[^\]]*)\] "
    r"stadata_num=(?P<sta_count>\d+) "
    r"stadata=\[(?P<sta_data>[^\]]*)\]"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract CCO MAC, ccodata and stadata from period_cmp log lines."
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="Log files, directories, or glob patterns to scan.",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="-",
        help="Output file path. Use '-' for stdout.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail immediately when a malformed record is found.",
    )
    return parser.parse_args()


def expand_inputs(raw_inputs):
    files = []
    seen = set()

    for raw_input in raw_inputs:
        path = Path(raw_input)

        if path.is_file():
            candidates = [path]
        elif path.is_dir():
            candidates = sorted(p for p in path.rglob("*.log") if p.is_file())
        else:
            candidates = [Path(item) for item in sorted(glob.glob(raw_input, recursive=True))]

        for candidate in candidates:
            resolved = candidate.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            files.append(candidate)

    return files


def parse_number_list(raw_text):
    if not raw_text.strip():
        return []

    return [int(item.strip()) for item in raw_text.split(",") if item.strip()]


def values_fit_int16(values):
    return all(-32768 <= value <= 32767 for value in values)


def extract_records(log_path, strict=False):
    records = []
    skipped = 0

    with log_path.open("r", encoding="utf-8", errors="ignore") as handle:
        for line_no, line in enumerate(handle, 1):
            match = RECORD_PATTERN.search(line)
            if match is None:
                continue

            try:
                cco_data = parse_number_list(match.group("cco_data"))
                sta_data = parse_number_list(match.group("sta_data"))
            except ValueError as exc:
                message = f"{log_path}:{line_no} invalid numeric token: {exc}"
                if strict:
                    raise ValueError(message) from exc
                print(f"WARN: {message}", file=sys.stderr)
                skipped += 1
                continue

            cco_count = int(match.group("cco_count"))
            sta_count = int(match.group("sta_count"))

            if len(cco_data) != cco_count:
                message = (
                    f"{log_path}:{line_no} ccodata count mismatch: "
                    f"expected {cco_count}, got {len(cco_data)}"
                )
                if strict:
                    raise ValueError(message)
                print(f"WARN: {message}", file=sys.stderr)
                skipped += 1
                continue

            if len(sta_data) != sta_count:
                message = (
                    f"{log_path}:{line_no} stadata count mismatch: "
                    f"expected {sta_count}, got {len(sta_data)}"
                )
                if strict:
                    raise ValueError(message)
                print(f"WARN: {message}", file=sys.stderr)
                skipped += 1
                continue

            if not values_fit_int16(cco_data):
                message = f"{log_path}:{line_no} ccodata contains value outside int16 range"
                if strict:
                    raise ValueError(message)
                print(f"WARN: {message}", file=sys.stderr)
                skipped += 1
                continue

            if not values_fit_int16(sta_data):
                message = f"{log_path}:{line_no} stadata contains value outside int16 range"
                if strict:
                    raise ValueError(message)
                print(f"WARN: {message}", file=sys.stderr)
                skipped += 1
                continue

            records.append({
                "source_log": str(log_path),
                "source_line": line_no,
                "cco_mac": match.group("cco_mac"),
                "cco_data": cco_data,
                "sta_data": sta_data,
            })

    return records, skipped


def write_record(handle, record):
    handle.write("AREA_FREQ_RECORD_V1\n")
    handle.write(f"source_log={record['source_log']}\n")
    handle.write(f"source_line={record['source_line']}\n")
    handle.write(f"cco_mac={record['cco_mac']}\n")
    handle.write(f"cco_data_count={len(record['cco_data'])}\n")
    handle.write("cco_data=")
    handle.write(",".join(str(value) for value in record["cco_data"]))
    handle.write("\n")
    handle.write(f"sta_data_count={len(record['sta_data'])}\n")
    handle.write("sta_data=")
    handle.write(",".join(str(value) for value in record["sta_data"]))
    handle.write("\n\n")


def main():
    args = parse_args()
    input_files = expand_inputs(args.inputs)

    if not input_files:
        print("No log files found.", file=sys.stderr)
        return 1

    output_handle = sys.stdout
    need_close = False

    if args.output != "-":
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_handle = output_path.open("w", encoding="utf-8", newline="\n")
        need_close = True

    total_records = 0
    total_files = 0
    total_skipped = 0

    try:
        for log_path in input_files:
            file_records = 0
            records, skipped = extract_records(log_path, strict=args.strict)
            for record in records:
                write_record(output_handle, record)
                file_records += 1
                total_records += 1

            total_skipped += skipped

            if file_records > 0:
                total_files += 1

            print(
                f"{log_path}: extracted {file_records} record(s), skipped {skipped}",
                file=sys.stderr,
            )
    finally:
        if need_close:
            output_handle.close()

    if total_records == 0:
        print("No period_cmp records found.", file=sys.stderr)
        return 1

    print(
        f"Done. files_with_records={total_files}, total_records={total_records}, "
        f"skipped_records={total_skipped}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
