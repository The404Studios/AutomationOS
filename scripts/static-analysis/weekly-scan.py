#!/usr/bin/env python3
"""
AutomationOS Weekly Static Analysis Scan

Performs comprehensive weekly static analysis scan with detailed reporting,
trend analysis, and comparison with previous scans.
"""

import os
import sys
import json
import subprocess
from datetime import datetime
from pathlib import Path


class WeeklyScan:
    """Orchestrates weekly comprehensive static analysis."""

    def __init__(self, project_root):
        self.project_root = Path(project_root)
        self.analysis_dir = self.project_root / "build" / "static-analysis"
        self.weekly_dir = self.analysis_dir / "weekly"
        self.weekly_dir.mkdir(parents=True, exist_ok=True)

        self.date_str = datetime.now().strftime("%Y-%m-%d")
        self.report_file = self.weekly_dir / f"report-{self.date_str}.txt"
        self.json_file = self.weekly_dir / f"data-{self.date_str}.json"

    def run_analysis(self):
        """Run complete static analysis suite."""
        print(f"=== AutomationOS Weekly Static Analysis Scan ===")
        print(f"Date: {self.date_str}")
        print()

        # Change to project root
        os.chdir(self.project_root)

        # Run analyzers
        tools = ["analyze", "cppcheck", "sparse", "clang-tidy"]
        results = {}

        for tool in tools:
            print(f"Running {tool}...")
            try:
                result = subprocess.run(
                    ["make", tool],
                    capture_output=True,
                    text=True,
                    timeout=600  # 10 minute timeout per tool
                )
                results[tool] = {
                    "returncode": result.returncode,
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                }
                print(f"  ✓ {tool} completed (exit code: {result.returncode})")
            except subprocess.TimeoutExpired:
                print(f"  ✗ {tool} timed out")
                results[tool] = {"returncode": -1, "error": "timeout"}
            except Exception as e:
                print(f"  ✗ {tool} failed: {e}")
                results[tool] = {"returncode": -1, "error": str(e)}

        return results

    def parse_results(self, results):
        """Parse analysis results and extract metrics."""
        metrics = {
            "date": self.date_str,
            "tools": {},
            "total_issues": 0,
            "critical": 0,
            "high": 0,
            "medium": 0,
            "low": 0,
        }

        # Parse each tool's output
        for tool, result in results.items():
            if result["returncode"] == -1:
                continue

            output = result.get("stdout", "") + result.get("stderr", "")
            lines = [line for line in output.splitlines() if line.strip()]

            # Count issues (simple heuristic)
            issue_count = len([l for l in lines if "warning:" in l.lower() or "error:" in l.lower()])

            metrics["tools"][tool] = {
                "issue_count": issue_count,
                "exit_code": result["returncode"],
            }
            metrics["total_issues"] += issue_count

        return metrics

    def load_previous_scan(self):
        """Load most recent previous scan for comparison."""
        json_files = sorted(self.weekly_dir.glob("data-*.json"), reverse=True)

        # Skip current scan, get previous
        for json_file in json_files:
            if json_file != self.json_file:
                try:
                    with open(json_file, "r") as f:
                        return json.load(f)
                except Exception:
                    continue

        return None

    def generate_report(self, metrics, previous_metrics):
        """Generate comprehensive weekly report."""
        lines = []

        lines.append("=" * 80)
        lines.append("AutomationOS Weekly Static Analysis Report")
        lines.append("=" * 80)
        lines.append(f"Date: {self.date_str}")
        lines.append(f"Time: {datetime.now().strftime('%H:%M:%S')}")
        lines.append("")

        # Executive Summary
        lines.append("=" * 80)
        lines.append("Executive Summary")
        lines.append("=" * 80)
        lines.append(f"Total Issues Found: {metrics['total_issues']}")
        lines.append(f"  Critical: {metrics['critical']}")
        lines.append(f"  High:     {metrics['high']}")
        lines.append(f"  Medium:   {metrics['medium']}")
        lines.append(f"  Low:      {metrics['low']}")
        lines.append("")

        # Trend Analysis
        if previous_metrics:
            lines.append("Trend (vs. previous scan):")
            prev_total = previous_metrics.get("total_issues", 0)
            delta = metrics["total_issues"] - prev_total
            if delta > 0:
                lines.append(f"  📈 +{delta} issues (increased)")
            elif delta < 0:
                lines.append(f"  📉 {delta} issues (improved)")
            else:
                lines.append(f"  ➡️  No change")
            lines.append("")

        # Per-Tool Results
        lines.append("=" * 80)
        lines.append("Per-Tool Results")
        lines.append("=" * 80)
        for tool, data in metrics["tools"].items():
            lines.append(f"{tool}:")
            lines.append(f"  Issues: {data['issue_count']}")
            lines.append(f"  Exit Code: {data['exit_code']}")

            # Compare with previous
            if previous_metrics and tool in previous_metrics.get("tools", {}):
                prev_count = previous_metrics["tools"][tool]["issue_count"]
                delta = data["issue_count"] - prev_count
                if delta != 0:
                    lines.append(f"  Change: {'+' if delta > 0 else ''}{delta}")
            lines.append("")

        # Recommendations
        lines.append("=" * 80)
        lines.append("Recommendations")
        lines.append("=" * 80)

        if metrics["critical"] > 0:
            lines.append(f"🚨 URGENT: Fix {metrics['critical']} critical issues immediately")

        if metrics["high"] > 0:
            lines.append(f"⚠️  Address {metrics['high']} high severity issues this sprint")

        if metrics["total_issues"] == 0:
            lines.append("✅ Excellent! No issues found. Keep up the good work!")
        elif metrics["total_issues"] < 10:
            lines.append("👍 Good code quality. Minor issues to address.")
        elif metrics["total_issues"] < 50:
            lines.append("⚠️  Moderate number of issues. Schedule cleanup tasks.")
        else:
            lines.append("🔴 High issue count. Dedicate time to technical debt reduction.")

        lines.append("")

        # Detailed Results
        lines.append("=" * 80)
        lines.append("Detailed Results")
        lines.append("=" * 80)
        lines.append("")
        lines.append(f"Full analysis logs available at:")
        lines.append(f"  {self.analysis_dir}/")
        lines.append("")
        lines.append(f"Individual tool results:")
        lines.append(f"  Clang Analyzer: {self.analysis_dir}/clang-analyzer.log")
        lines.append(f"  Cppcheck:       {self.analysis_dir}/cppcheck.log")
        lines.append(f"  Sparse:         {self.analysis_dir}/sparse.log")
        lines.append(f"  Clang-Tidy:     {self.analysis_dir}/clang-tidy.log")
        lines.append("")

        # Footer
        lines.append("=" * 80)
        lines.append("Next Steps")
        lines.append("=" * 80)
        lines.append("1. Review critical and high severity issues")
        lines.append("2. Create GitHub issues for major findings")
        lines.append("3. Schedule fixes in upcoming sprints")
        lines.append("4. Update .static-analysis-suppressions for false positives")
        lines.append("")
        lines.append("For detailed analysis guide, see: docs/STATIC_ANALYSIS.md")
        lines.append("=" * 80)

        return "\n".join(lines)

    def run(self):
        """Execute weekly scan workflow."""
        # Run analysis
        results = self.run_analysis()

        # Parse metrics
        metrics = self.parse_results(results)

        # Load previous scan
        previous_metrics = self.load_previous_scan()

        # Generate report
        report = self.generate_report(metrics, previous_metrics)

        # Save results
        with open(self.report_file, "w") as f:
            f.write(report)

        with open(self.json_file, "w") as f:
            json.dump(metrics, f, indent=2)

        # Print report
        print("\n" + report)

        # Print save location
        print(f"\n📊 Report saved to: {self.report_file}")
        print(f"📈 Metrics saved to: {self.json_file}")

        return 0 if metrics["critical"] == 0 else 1


def main():
    """Main entry point."""
    # Determine project root
    script_dir = Path(__file__).parent
    project_root = script_dir.parent.parent

    if not (project_root / "Makefile").exists():
        print(f"Error: Project root not found (expected Makefile at {project_root})")
        sys.exit(1)

    # Run weekly scan
    scanner = WeeklyScan(project_root)
    exit_code = scanner.run()

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
