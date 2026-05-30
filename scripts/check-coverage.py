#!/usr/bin/env python3
"""
Check coverage thresholds and fail CI if not met.

Usage:
    python3 scripts/check-coverage.py --info-file coverage_filtered.info --threshold 80
    python3 scripts/check-coverage.py --threshold 75 --branch-threshold 65
"""

import sys
import re
import argparse
from pathlib import Path


def parse_coverage_info(info_file):
    """Parse lcov .info file for coverage percentages."""
    if not Path(info_file).exists():
        print(f"ERROR: Coverage file '{info_file}' not found")
        return None, None, None, None

    with open(info_file, 'r') as f:
        content = f.read()

    # Extract line coverage
    lines_found_matches = re.findall(r'LF:(\d+)', content)
    lines_hit_matches = re.findall(r'LH:(\d+)', content)
    lines_found = sum(int(x) for x in lines_found_matches)
    lines_hit = sum(int(x) for x in lines_hit_matches)
    line_coverage = (lines_hit / lines_found * 100) if lines_found > 0 else 0

    # Extract function coverage
    funcs_found_matches = re.findall(r'FNF:(\d+)', content)
    funcs_hit_matches = re.findall(r'FNH:(\d+)', content)
    funcs_found = sum(int(x) for x in funcs_found_matches)
    funcs_hit = sum(int(x) for x in funcs_hit_matches)
    func_coverage = (funcs_hit / funcs_found * 100) if funcs_found > 0 else 0

    # Extract branch coverage
    branches_found_matches = re.findall(r'BRF:(\d+)', content)
    branches_hit_matches = re.findall(r'BRH:(\d+)', content)
    branches_found = sum(int(x) for x in branches_found_matches)
    branches_hit = sum(int(x) for x in branches_hit_matches)
    branch_coverage = (branches_hit / branches_found * 100) if branches_found > 0 else 0

    return line_coverage, func_coverage, branch_coverage, {
        'lines_found': lines_found,
        'lines_hit': lines_hit,
        'funcs_found': funcs_found,
        'funcs_hit': funcs_hit,
        'branches_found': branches_found,
        'branches_hit': branches_hit,
    }


def print_coverage_summary(line_cov, func_cov, branch_cov, stats):
    """Print a nice coverage summary."""
    print("=" * 60)
    print("  Coverage Report Summary")
    print("=" * 60)
    print()
    print(f"Line Coverage:     {line_cov:6.2f}%  ({stats['lines_hit']}/{stats['lines_found']} lines)")
    print(f"Function Coverage: {func_cov:6.2f}%  ({stats['funcs_hit']}/{stats['funcs_found']} functions)")
    print(f"Branch Coverage:   {branch_cov:6.2f}%  ({stats['branches_hit']}/{stats['branches_found']} branches)")
    print()


def main():
    parser = argparse.ArgumentParser(
        description='Check coverage thresholds for CI/CD enforcement',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Check 80% line coverage
  python3 scripts/check-coverage.py --threshold 80

  # Check both line and branch coverage
  python3 scripts/check-coverage.py --threshold 80 --branch-threshold 70

  # Specify custom info file
  python3 scripts/check-coverage.py --info-file build/coverage.info --threshold 75
        """
    )
    parser.add_argument('--info-file', default='coverage_filtered.info',
                       help='Coverage info file (default: coverage_filtered.info)')
    parser.add_argument('--threshold', type=float, default=80.0,
                       help='Minimum line coverage percentage (default: 80.0)')
    parser.add_argument('--branch-threshold', type=float, default=None,
                       help='Minimum branch coverage percentage (optional)')
    parser.add_argument('--func-threshold', type=float, default=None,
                       help='Minimum function coverage percentage (optional)')
    parser.add_argument('--strict', action='store_true',
                       help='Fail if any threshold is not met (default: warn only)')
    args = parser.parse_args()

    # Parse coverage data
    line_cov, func_cov, branch_cov, stats = parse_coverage_info(args.info_file)

    if line_cov is None:
        print("ERROR: Failed to parse coverage data")
        sys.exit(1)

    # Print summary
    print_coverage_summary(line_cov, func_cov, branch_cov, stats)

    # Check thresholds
    failed = False
    warnings = []

    # Line coverage check
    if line_cov < args.threshold:
        msg = f"Line coverage {line_cov:.2f}% is below threshold {args.threshold}%"
        if args.strict:
            print(f"❌ FAIL: {msg}")
            failed = True
        else:
            warnings.append(msg)
    else:
        print(f"✅ PASS: Line coverage {line_cov:.2f}% meets threshold {args.threshold}%")

    # Branch coverage check
    if args.branch_threshold is not None:
        if branch_cov < args.branch_threshold:
            msg = f"Branch coverage {branch_cov:.2f}% is below threshold {args.branch_threshold}%"
            if args.strict:
                print(f"❌ FAIL: {msg}")
                failed = True
            else:
                warnings.append(msg)
        else:
            print(f"✅ PASS: Branch coverage {branch_cov:.2f}% meets threshold {args.branch_threshold}%")

    # Function coverage check
    if args.func_threshold is not None:
        if func_cov < args.func_threshold:
            msg = f"Function coverage {func_cov:.2f}% is below threshold {args.func_threshold}%"
            if args.strict:
                print(f"❌ FAIL: {msg}")
                failed = True
            else:
                warnings.append(msg)
        else:
            print(f"✅ PASS: Function coverage {func_cov:.2f}% meets threshold {args.func_threshold}%")

    # Print warnings
    if warnings:
        print()
        print("⚠️  Warnings:")
        for warning in warnings:
            print(f"  - {warning}")
        print()
        print("Note: Use --strict to fail CI on warnings")

    # Exit
    print()
    print("=" * 60)
    if failed:
        print("❌ Coverage check FAILED")
        print("=" * 60)
        sys.exit(1)
    elif warnings:
        print("⚠️  Coverage check passed with warnings")
        print("=" * 60)
        sys.exit(0)
    else:
        print("✅ Coverage check PASSED")
        print("=" * 60)
        sys.exit(0)


if __name__ == '__main__':
    main()
