#!/usr/bin/env python3
"""
AutomationOS Kernel Test Result Parser

This script parses the output from kernel tests and generates reports in various formats:
- Console summary
- JUnit XML (for CI/CD integration)
- JSON (for programmatic access)
- HTML report (for human viewing)
"""

import sys
import re
import json
import xml.etree.ElementTree as ET
from datetime import datetime
from typing import List, Dict, Tuple

class TestResult:
    """Represents a single test result"""
    def __init__(self, suite: str, name: str, status: str, cycles: int = 0, message: str = ""):
        self.suite = suite
        self.name = name
        self.status = status  # "PASS", "FAIL", "SKIP"
        self.cycles = cycles
        self.message = message

    def __repr__(self):
        return f"TestResult({self.suite}.{self.name}: {self.status})"

class TestSummary:
    """Aggregated test statistics"""
    def __init__(self):
        self.total = 0
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.total_cycles = 0
        self.results: List[TestResult] = []

    def add_result(self, result: TestResult):
        self.results.append(result)
        self.total += 1
        self.total_cycles += result.cycles

        if result.status == "PASS":
            self.passed += 1
        elif result.status == "FAIL":
            self.failed += 1
        elif result.status == "SKIP":
            self.skipped += 1

    def pass_rate(self) -> float:
        if self.total == 0:
            return 0.0
        return (self.passed / self.total) * 100.0

def parse_test_output(log_file: str) -> TestSummary:
    """Parse kernel test output and extract results"""
    summary = TestSummary()

    # Regular expressions for parsing
    run_pattern = re.compile(r'\[RUN \] (\w+)\.(\w+)')
    ok_pattern = re.compile(r'\[ OK \] (\w+)\.(\w+) \((\d+) cycles\)')
    fail_pattern = re.compile(r'\[FAIL\] (\w+)\.(\w+)')
    skip_pattern = re.compile(r'\[SKIP\] (\w+)\.(\w+)')
    fail_msg_pattern = re.compile(r'^\s+(.+):(\d+): (.+)$')

    current_test = None
    current_message = ""

    try:
        with open(log_file, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()

                # Match test run
                match = run_pattern.search(line)
                if match:
                    suite, name = match.groups()
                    current_test = (suite, name)
                    current_message = ""
                    continue

                # Match test pass
                match = ok_pattern.search(line)
                if match:
                    suite, name, cycles = match.groups()
                    result = TestResult(suite, name, "PASS", int(cycles))
                    summary.add_result(result)
                    current_test = None
                    continue

                # Match test fail
                match = fail_pattern.search(line)
                if match:
                    suite, name = match.groups()
                    result = TestResult(suite, name, "FAIL", message=current_message)
                    summary.add_result(result)
                    current_test = None
                    continue

                # Match test skip
                match = skip_pattern.search(line)
                if match:
                    suite, name = match.groups()
                    result = TestResult(suite, name, "SKIP")
                    summary.add_result(result)
                    current_test = None
                    continue

                # Match failure message
                match = fail_msg_pattern.search(line)
                if match and current_test:
                    file, line_num, msg = match.groups()
                    current_message = f"{file}:{line_num}: {msg}"
                    continue

    except FileNotFoundError:
        print(f"Error: File '{log_file}' not found", file=sys.stderr)
        sys.exit(1)

    return summary

def print_console_summary(summary: TestSummary):
    """Print human-readable summary to console"""
    print("\n" + "="*60)
    print(" KERNEL TEST RESULTS SUMMARY")
    print("="*60)
    print(f"Total Tests:   {summary.total}")
    print(f"Passed:        {summary.passed} ({summary.pass_rate():.1f}%)")
    print(f"Failed:        {summary.failed}")
    print(f"Skipped:       {summary.skipped}")
    print(f"Total Cycles:  {summary.total_cycles:,}")
    print("="*60)

    if summary.failed > 0:
        print("\nFAILED TESTS:")
        for result in summary.results:
            if result.status == "FAIL":
                print(f"  ❌ {result.suite}.{result.name}")
                if result.message:
                    print(f"     {result.message}")

    # Group by suite
    suites: Dict[str, List[TestResult]] = {}
    for result in summary.results:
        if result.suite not in suites:
            suites[result.suite] = []
        suites[result.suite].append(result)

    print("\nRESULTS BY SUITE:")
    for suite_name, results in sorted(suites.items()):
        passed = sum(1 for r in results if r.status == "PASS")
        failed = sum(1 for r in results if r.status == "FAIL")
        skipped = sum(1 for r in results if r.status == "SKIP")
        total = len(results)
        status = "✅" if failed == 0 else "❌"

        print(f"  {status} {suite_name:15s} {passed:3d}/{total:3d} passed", end="")
        if failed > 0:
            print(f" ({failed} failed)", end="")
        if skipped > 0:
            print(f" ({skipped} skipped)", end="")
        print()

    print()

def generate_junit_xml(summary: TestSummary, output_file: str):
    """Generate JUnit XML format for CI/CD integration"""
    testsuites = ET.Element('testsuites')
    testsuites.set('tests', str(summary.total))
    testsuites.set('failures', str(summary.failed))
    testsuites.set('skipped', str(summary.skipped))
    testsuites.set('time', str(summary.total_cycles / 1000000))  # Convert to seconds (approx)

    # Group by suite
    suites: Dict[str, List[TestResult]] = {}
    for result in summary.results:
        if result.suite not in suites:
            suites[result.suite] = []
        suites[result.suite].append(result)

    # Create testsuite elements
    for suite_name, results in suites.items():
        testsuite = ET.SubElement(testsuites, 'testsuite')
        testsuite.set('name', suite_name)
        testsuite.set('tests', str(len(results)))
        testsuite.set('failures', str(sum(1 for r in results if r.status == "FAIL")))
        testsuite.set('skipped', str(sum(1 for r in results if r.status == "SKIP")))

        for result in results:
            testcase = ET.SubElement(testsuite, 'testcase')
            testcase.set('classname', result.suite)
            testcase.set('name', result.name)
            testcase.set('time', str(result.cycles / 1000000))

            if result.status == "FAIL":
                failure = ET.SubElement(testcase, 'failure')
                failure.set('message', result.message or "Test failed")
                failure.text = result.message

            elif result.status == "SKIP":
                skipped = ET.SubElement(testcase, 'skipped')

    # Write to file
    tree = ET.ElementTree(testsuites)
    tree.write(output_file, encoding='utf-8', xml_declaration=True)
    print(f"JUnit XML report written to: {output_file}")

def generate_json_report(summary: TestSummary, output_file: str):
    """Generate JSON format report"""
    report = {
        'timestamp': datetime.now().isoformat(),
        'summary': {
            'total': summary.total,
            'passed': summary.passed,
            'failed': summary.failed,
            'skipped': summary.skipped,
            'pass_rate': summary.pass_rate(),
            'total_cycles': summary.total_cycles
        },
        'results': []
    }

    for result in summary.results:
        report['results'].append({
            'suite': result.suite,
            'name': result.name,
            'status': result.status,
            'cycles': result.cycles,
            'message': result.message
        })

    with open(output_file, 'w') as f:
        json.dump(report, f, indent=2)

    print(f"JSON report written to: {output_file}")

def generate_html_report(summary: TestSummary, output_file: str):
    """Generate HTML report"""
    html = f"""<!DOCTYPE html>
<html>
<head>
    <title>AutomationOS Kernel Test Results</title>
    <style>
        body {{
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
            background: #f5f5f5;
        }}
        .header {{
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            border-radius: 10px;
            margin-bottom: 20px;
        }}
        .stats {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }}
        .stat-card {{
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        .stat-value {{
            font-size: 36px;
            font-weight: bold;
            margin: 10px 0;
        }}
        .stat-label {{
            color: #666;
            font-size: 14px;
        }}
        .pass {{ color: #10b981; }}
        .fail {{ color: #ef4444; }}
        .skip {{ color: #f59e0b; }}
        table {{
            width: 100%;
            background: white;
            border-radius: 8px;
            overflow: hidden;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        th {{
            background: #667eea;
            color: white;
            padding: 12px;
            text-align: left;
        }}
        td {{
            padding: 12px;
            border-bottom: 1px solid #eee;
        }}
        tr:hover {{
            background: #f9fafb;
        }}
        .suite-name {{
            font-weight: bold;
            color: #667eea;
        }}
    </style>
</head>
<body>
    <div class="header">
        <h1>🧪 AutomationOS Kernel Test Results</h1>
        <p>Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
    </div>

    <div class="stats">
        <div class="stat-card">
            <div class="stat-label">Total Tests</div>
            <div class="stat-value">{summary.total}</div>
        </div>
        <div class="stat-card">
            <div class="stat-label">Passed</div>
            <div class="stat-value pass">{summary.passed}</div>
            <div class="stat-label">{summary.pass_rate():.1f}% pass rate</div>
        </div>
        <div class="stat-card">
            <div class="stat-label">Failed</div>
            <div class="stat-value fail">{summary.failed}</div>
        </div>
        <div class="stat-card">
            <div class="stat-label">Skipped</div>
            <div class="stat-value skip">{summary.skipped}</div>
        </div>
    </div>

    <table>
        <thead>
            <tr>
                <th>Suite</th>
                <th>Test</th>
                <th>Status</th>
                <th>Cycles</th>
                <th>Message</th>
            </tr>
        </thead>
        <tbody>
"""

    for result in summary.results:
        status_class = result.status.lower()
        status_icon = {"PASS": "✅", "FAIL": "❌", "SKIP": "⏭️"}.get(result.status, "❓")

        html += f"""
            <tr>
                <td class="suite-name">{result.suite}</td>
                <td>{result.name}</td>
                <td class="{status_class}">{status_icon} {result.status}</td>
                <td>{result.cycles:,}</td>
                <td>{result.message if result.message else '-'}</td>
            </tr>
"""

    html += """
        </tbody>
    </table>
</body>
</html>
"""

    with open(output_file, 'w') as f:
        f.write(html)

    print(f"HTML report written to: {output_file}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_test_results.py <kernel_log_file> [options]")
        print("\nOptions:")
        print("  --junit <file>    Generate JUnit XML output")
        print("  --json <file>     Generate JSON output")
        print("  --html <file>     Generate HTML output")
        print("  --quiet           Only print summary")
        sys.exit(1)

    log_file = sys.argv[1]

    # Parse test results
    print(f"Parsing test results from: {log_file}")
    summary = parse_test_output(log_file)

    # Print console summary
    print_console_summary(summary)

    # Generate additional reports
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == '--junit' and i + 1 < len(args):
            generate_junit_xml(summary, args[i + 1])
            i += 2
        elif args[i] == '--json' and i + 1 < len(args):
            generate_json_report(summary, args[i + 1])
            i += 2
        elif args[i] == '--html' and i + 1 < len(args):
            generate_html_report(summary, args[i + 1])
            i += 2
        else:
            i += 1

    # Exit with error code if tests failed
    sys.exit(0 if summary.failed == 0 else 1)

if __name__ == '__main__':
    main()
