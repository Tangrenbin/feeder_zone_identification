#!/usr/bin/env python3

import argparse
import glob
import re
import sys
from pathlib import Path

BLOCK_MARKER = "area_record_frequency_diff params:"
CCO_MAC_PATTERN = re.compile(
    r"CcoMac=(?P<cco_mac>(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})"
)
DATA_PATTERN = re.compile(
    r"(?P<label>CcoData|StaData)=\[(?P<data>[^\]]*)\]\s*count=(?P<count>\d+)"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract CCO MAC, CcoData and StaData from area_record_frequency_diff log blocks."
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
    supported_patterns = ("*.log", "*.txt", "*.TXT")

    for raw_input in raw_inputs:
        path = Path(raw_input)

        if path.is_file():
            candidates = [path]
        elif path.is_dir():
            candidates = sorted(
                {
                    p
                    for pattern in supported_patterns
                    for p in path.rglob(pattern)
                    if p.is_file()
                }
            )
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


def normalize_cco_mac(raw_text):
    return raw_text.replace(":", "").upper()


def report_record_error(message, strict=False):
    if strict:
        raise ValueError(message)

    print(f"WARN: {message}", file=sys.stderr)
    return None, 1


def finalize_record(log_path, block):
    source_line = block["cco_mac_line"] or block["source_line"]
    missing_fields = []

    if block["cco_mac"] is None:
        missing_fields.append("CcoMac")
    if block["cco_data"] is None:
        missing_fields.append("CcoData")
    if block["sta_data"] is None:
        missing_fields.append("StaData")

    if missing_fields:
        return report_record_error(
            f"{log_path}:{block['source_line']} incomplete block missing {', '.join(missing_fields)}",
            strict=block["strict"],
        )

    try:
        cco_data = parse_number_list(block["cco_data"])
    except ValueError as exc:
        return report_record_error(
            f"{log_path}:{block['cco_data_line']} invalid CcoData numeric token: {exc}",
            strict=block["strict"],
        )

    try:
        sta_data = parse_number_list(block["sta_data"])
    except ValueError as exc:
        return report_record_error(
            f"{log_path}:{block['sta_data_line']} invalid StaData numeric token: {exc}",
            strict=block["strict"],
        )

    cco_count = int(block["cco_count"])
    sta_count = int(block["sta_count"])

    if len(cco_data) != cco_count:
        return report_record_error(
            f"{log_path}:{block['cco_data_line']} CcoData count mismatch: "
            f"expected {cco_count}, got {len(cco_data)}",
            strict=block["strict"],
        )

    if len(sta_data) != sta_count:
        return report_record_error(
            f"{log_path}:{block['sta_data_line']} StaData count mismatch: "
            f"expected {sta_count}, got {len(sta_data)}",
            strict=block["strict"],
        )

    if not values_fit_int16(cco_data):
        return report_record_error(
            f"{log_path}:{block['cco_data_line']} CcoData contains value outside int16 range",
            strict=block["strict"],
        )

    if not values_fit_int16(sta_data):
        return report_record_error(
            f"{log_path}:{block['sta_data_line']} StaData contains value outside int16 range",
            strict=block["strict"],
        )

    return {
        "source_log": str(log_path),
        "source_line": source_line,
        "cco_mac": normalize_cco_mac(block["cco_mac"]),
        "cco_data": cco_data,
        "sta_data": sta_data,
    }, 0


def extract_records(log_path, strict=False):
    records = []
    skipped = 0
    current_block = None

    with log_path.open("r", encoding="utf-8", errors="ignore") as handle:
        for line_no, line in enumerate(handle, 1):
            if BLOCK_MARKER in line:
                if current_block is not None:
                    record, skipped_delta = finalize_record(log_path, current_block)
                    skipped += skipped_delta
                    if record is not None:
                        records.append(record)

                current_block = {
                    "source_line": line_no,
                    "strict": strict,
                    "cco_mac": None,
                    "cco_mac_line": None,
                    "cco_data": None,
                    "cco_data_line": None,
                    "cco_count": None,
                    "sta_data": None,
                    "sta_data_line": None,
                    "sta_count": None,
                }

            if current_block is None:
                continue

            match = CCO_MAC_PATTERN.search(line)
            if match is not None:
                current_block["cco_mac"] = match.group("cco_mac")
                current_block["cco_mac_line"] = line_no
                continue

            match = DATA_PATTERN.search(line)
            if match is None:
                continue

            label = match.group("label")
            if label == "CcoData":
                current_block["cco_data"] = match.group("data")
                current_block["cco_data_line"] = line_no
                current_block["cco_count"] = match.group("count")
            else:
                current_block["sta_data"] = match.group("data")
                current_block["sta_data_line"] = line_no
                current_block["sta_count"] = match.group("count")

    if current_block is not None:
        record, skipped_delta = finalize_record(log_path, current_block)
        skipped += skipped_delta
        if record is not None:
            records.append(record)

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
        print("No area_record_frequency_diff records found.", file=sys.stderr)
        return 1

    print(
        f"Done. files_with_records={total_files}, total_records={total_records}, "
        f"skipped_records={total_skipped}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
