#!/usr/bin/env python3
"""
AutomationOS Static Analysis Report Generator

Aggregates results from multiple static analysis tools and generates
a unified summary report with severity classification.
"""

import os
import sys
import re
from datetime import datetime
from pathlib import Path

# Severity levels
CRITICAL = "Critical"
HIGH = "High"
MEDIUM = "Medium"
LOW = "Low"

# Issue patterns for each tool
PATTERNS = {
    "clang-analyzer": {
        "critical": [
            r"use of memory after it is freed",
            r"dereference of null pointer",
            r"use of uninitialized value",
            r"double free",
        ],
        "high": [
            r"memory leak",
            r"resource leak",
            r"buffer overflow",
            r"division by zero",
        ],
        "medium": [
            r"dead assignment",
            r"unused variable",
            r"logic error",
        ],
    },
    "cppcheck": {
        "critical": [
            r"nullPointer",
            r"uninitvar",
            r"bufferAccessOutOfBounds",
        ],
        "high": [
            r"memleak",
            r"resourceLeak",
            r"useAfterFree",
        ],
        "medium": [
            r"unusedVariable",
            r"redundantAssignment",
            r"clarifyCondition",
        ],
    },
    "sparse": {
        "critical": [
            r"context imbalance",
            r"dereference of noderef expression",
        ],
        "high": [
            r"incorrect type",
            r"address space mismatch",
        ],
        "medium": [
            r"warning:",
        ],
    },
    "clang-tidy": {
        "critical": [
            r"bugprone-use-after-move",
            r"cert-dcl50-cpp",
        ],
        "high": [
            r"bugprone-",
            r"cert-",
            r"concurrency-",
        ],
        "medium": [
            r"readability-",
            r"performance-",
        ],
    },
}


class StaticAnalysisReport:
    """Aggregates and reports static analysis results."""

    def __init__(self, analysis_dir):
        self.analysis_dir = Path(analysis_dir)
        self.issues = {
            CRITICAL: [],
            HIGH: [],
            MEDIUM: [],
            LOW: [],
        }
        self.tool_counts = {}

    def parse_log(self, log_file, tool_name):
        """Parse a log file and categorize issues by severity."""
        if not log_file.exists():
            return

        with open(log_file, "r") as f:
            content = f.read()

        # Count issues by severity
        tool_issues = {CRITICAL: 0, HIGH: 0, MEDIUM: 0, LOW: 0}

        for line in content.splitlines():
            severity = self._classify_issue(line, tool_name)
            if severity:
                tool_issues[severity] += 1
                self.issues[severity].append({
                    "tool": tool_name,
                    "line": line.strip(),
                })

        self.tool_counts[tool_name] = tool_issues

    def _classify_issue(self, line, tool_name):
        """Classify issue severity based on patterns."""
        if not line or line.startswith("==="):
            return None

        patterns = PATTERNS.get(tool_name, {})

        # Check critical patterns first
        for pattern in patterns.get("critical", []):
            if re.search(pattern, line, re.IGNORECASE):
                return CRITICAL

        # Check high severity
        for pattern in patterns.get("high", []):
            if re.search(pattern, line, re.IGNORECASE):
                return HIGH

        # Check medium severity
        for pattern in patterns.get("medium", []):
            if re.search(pattern, line, re.IGNORECASE):
                return MEDIUM

        # Generic issue (low severity)
        if re.search(r"\berror\b|\bwarning\b", line, re.IGNORECASE):
            return LOW

        return None

    def generate_summary(self):
        """Generate summary text report."""
        summary_lines = []

        summary_lines.append("=" * 70)
        summary_lines.append("AutomationOS Static Analysis Summary")
        summary_lines.append("=" * 70)
        summary_lines.append(f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        summary_lines.append("")

        # Overall summary
        summary_lines.append("Issue Summary:")
        summary_lines.append(f"  Critical: {len(self.issues[CRITICAL])}")
        summary_lines.append(f"  High:     {len(self.issues[HIGH])}")
        summary_lines.append(f"  Medium:   {len(self.issues[MEDIUM])}")
        summary_lines.append(f"  Low:      {len(self.issues[LOW])}")
        summary_lines.append("")

        # Per-tool breakdown
        summary_lines.append("Per-Tool Breakdown:")
        for tool, counts in self.tool_counts.items():
            summary_lines.append(f"  {tool}:")
            for severity, count in counts.items():
                if count > 0:
                    summary_lines.append(f"    {severity}: {count}")
        summary_lines.append("")

        # Critical issues detail
        if self.issues[CRITICAL]:
            summary_lines.append("=" * 70)
            summary_lines.append("CRITICAL ISSUES (MUST FIX):")
            summary_lines.append("=" * 70)
            for issue in self.issues[CRITICAL][:10]:  # Show first 10
                summary_lines.append(f"[{issue['tool']}] {issue['line']}")
            if len(self.issues[CRITICAL]) > 10:
                summary_lines.append(f"... and {len(self.issues[CRITICAL]) - 10} more")
            summary_lines.append("")

        # High severity issues detail
        if self.issues[HIGH]:
            summary_lines.append("=" * 70)
            summary_lines.append("HIGH SEVERITY ISSUES:")
            summary_lines.append("=" * 70)
            for issue in self.issues[HIGH][:10]:  # Show first 10
                summary_lines.append(f"[{issue['tool']}] {issue['line']}")
            if len(self.issues[HIGH]) > 10:
                summary_lines.append(f"... and {len(self.issues[HIGH]) - 10} more")
            summary_lines.append("")

        # Status assessment
        summary_lines.append("=" * 70)
        summary_lines.append("Assessment:")
        summary_lines.append("=" * 70)
        if len(self.issues[CRITICAL]) == 0 and len(self.issues[HIGH]) == 0:
            summary_lines.append("✓ PASSED - No critical or high severity issues found")
        elif len(self.issues[CRITICAL]) > 0:
            summary_lines.append(f"✗ FAILED - {len(self.issues[CRITICAL])} critical issues must be fixed")
        else:
            summary_lines.append(f"⚠ WARNING - {len(self.issues[HIGH])} high severity issues found")
        summary_lines.append("")

        summary_lines.append("Full results: build/static-analysis/latest-scan.txt")
        summary_lines.append("=" * 70)

        return "\n".join(summary_lines)

    def save_summary(self):
        """Save summary to file."""
        summary_file = self.analysis_dir / "summary.txt"
        with open(summary_file, "w") as f:
            f.write(self.generate_summary())
        print(f"Summary saved to: {summary_file}")

    def get_exit_code(self):
        """Return appropriate exit code based on issues found."""
        if len(self.issues[CRITICAL]) > 0:
            return 2  # Critical failure
        elif len(self.issues[HIGH]) > 0:
            return 1  # Warning
        return 0  # Success


def main():
    """Main entry point."""
    # Determine analysis directory
    if len(sys.argv) > 1:
        analysis_dir = Path(sys.argv[1])
    else:
        # Default to build/static-analysis
        script_dir = Path(__file__).parent
        project_root = script_dir.parent.parent
        analysis_dir = project_root / "build" / "static-analysis"

    if not analysis_dir.exists():
        print(f"Error: Analysis directory not found: {analysis_dir}")
        sys.exit(1)

    # Create report
    report = StaticAnalysisReport(analysis_dir)

    # Parse all log files
    log_files = {
        "clang-analyzer": analysis_dir / "clang-analyzer.log",
        "cppcheck": analysis_dir / "cppcheck.log",
        "sparse": analysis_dir / "sparse.log",
        "clang-tidy": analysis_dir / "clang-tidy.log",
    }

    for tool_name, log_file in log_files.items():
        if log_file.exists():
            report.parse_log(log_file, tool_name)
        else:
            print(f"Warning: {tool_name} log not found at {log_file}")

    # Generate and save summary
    report.save_summary()

    # Print summary to stdout
    print("\n" + report.generate_summary())

    # Return appropriate exit code
    sys.exit(report.get_exit_code())


if __name__ == "__main__":
    main()
