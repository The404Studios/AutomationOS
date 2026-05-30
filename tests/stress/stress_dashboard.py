#!/usr/bin/env python3
"""
Stress Test Dashboard
Analyzes test results and generates visual reports
"""

import re
import json
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple

class StressTestAnalyzer:
    def __init__(self):
        self.vulnerabilities = []
        self.breaking_points = {}
        self.test_results = []
        self.metrics = {
            'passed': 0,
            'failed': 0,
            'critical_issues': 0,
            'warnings': 0
        }

    def parse_log_file(self, log_path: str) -> Dict:
        """Parse a stress test log file"""
        results = {
            'tests': [],
            'vulnerabilities': [],
            'breaking_points': {}
        }

        try:
            with open(log_path, 'r') as f:
                content = f.read()

            # Parse test results
            test_pattern = r'\[(PASS|FAIL)\] (.+?)$'
            for match in re.finditer(test_pattern, content, re.MULTILINE):
                status, test_name = match.groups()
                results['tests'].append({
                    'name': test_name,
                    'status': status,
                    'passed': status == 'PASS'
                })

            # Parse breaking points
            bp_pattern = r'Breaking Point: (\d+)'
            for match in re.finditer(bp_pattern, content):
                value = int(match.group(1))
                results['breaking_points']['generic'] = value

            # Parse critical issues
            if 'CRITICAL:' in content:
                critical_pattern = r'CRITICAL: (.+?)$'
                for match in re.finditer(critical_pattern, content, re.MULTILINE):
                    results['vulnerabilities'].append({
                        'severity': 'CRITICAL',
                        'description': match.group(1)
                    })

            # Parse warnings
            if 'WARNING:' in content:
                warning_pattern = r'WARNING: (.+?)$'
                for match in re.finditer(warning_pattern, content, re.MULTILINE):
                    results['vulnerabilities'].append({
                        'severity': 'WARNING',
                        'description': match.group(1)
                    })

        except FileNotFoundError:
            print(f"Warning: Log file not found: {log_path}")

        return results

    def analyze_all_logs(self):
        """Analyze all available log files"""
        log_dir = Path('.')
        log_files = list(log_dir.glob('*_results.log'))

        if not log_files:
            print("No log files found. Run tests first with 'make test-all'")
            return

        print("Analyzing test results...")

        for log_file in log_files:
            print(f"  - {log_file.name}")
            results = self.parse_log_file(str(log_file))

            for test in results['tests']:
                self.test_results.append(test)
                if test['passed']:
                    self.metrics['passed'] += 1
                else:
                    self.metrics['failed'] += 1

            for vuln in results['vulnerabilities']:
                self.vulnerabilities.append(vuln)
                if vuln['severity'] == 'CRITICAL':
                    self.metrics['critical_issues'] += 1
                else:
                    self.metrics['warnings'] += 1

            self.breaking_points.update(results['breaking_points'])

    def generate_summary_report(self) -> str:
        """Generate a text summary report"""
        report = []
        report.append("=" * 60)
        report.append("STRESS TEST SUMMARY REPORT")
        report.append("=" * 60)
        report.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("")

        # Test metrics
        report.append("TEST METRICS")
        report.append("-" * 60)
        total_tests = self.metrics['passed'] + self.metrics['failed']
        if total_tests > 0:
            pass_rate = (self.metrics['passed'] / total_tests) * 100
            report.append(f"Total Tests:      {total_tests}")
            report.append(f"Passed:           {self.metrics['passed']} ({pass_rate:.1f}%)")
            report.append(f"Failed:           {self.metrics['failed']}")
        else:
            report.append("No tests found. Run 'make test-all' first.")
        report.append("")

        # Security issues
        report.append("SECURITY ISSUES")
        report.append("-" * 60)
        report.append(f"Critical Issues:  {self.metrics['critical_issues']}")
        report.append(f"Warnings:         {self.metrics['warnings']}")
        report.append("")

        # Vulnerabilities
        if self.vulnerabilities:
            report.append("VULNERABILITIES FOUND")
            report.append("-" * 60)
            for i, vuln in enumerate(self.vulnerabilities[:10], 1):
                severity_marker = "🔴" if vuln['severity'] == 'CRITICAL' else "🟡"
                report.append(f"{i}. {severity_marker} [{vuln['severity']}] {vuln['description']}")

            if len(self.vulnerabilities) > 10:
                report.append(f"... and {len(self.vulnerabilities) - 10} more")
            report.append("")

        # Breaking points
        if self.breaking_points:
            report.append("BREAKING POINTS")
            report.append("-" * 60)
            for resource, value in self.breaking_points.items():
                report.append(f"{resource:20s}: {value:,}")
            report.append("")

        # Recommendations
        report.append("PRIORITY RECOMMENDATIONS")
        report.append("-" * 60)

        if self.metrics['critical_issues'] > 0:
            report.append("🔴 IMMEDIATE ACTION REQUIRED:")
            report.append("   1. Fix double-free detection")
            report.append("   2. Add heap canaries/guards")
            report.append("   3. Fix integer overflow vulnerabilities")
            report.append("")

        if self.metrics['failed'] > 0:
            report.append("🟡 RELIABILITY IMPROVEMENTS:")
            report.append("   1. Implement dynamic process table")
            report.append("   2. Add OOM killer instead of panic")
            report.append("   3. Fix TOCTOU race conditions")
            report.append("")

        report.append("Performance Optimization:")
        report.append("   1. Per-CPU heap caches")
        report.append("   2. Lock-free fast paths")
        report.append("   3. Reduce lock contention")
        report.append("")

        # Overall grade
        report.append("=" * 60)
        grade = self._calculate_grade()
        report.append(f"OVERALL SYSTEM GRADE: {grade}")
        report.append("=" * 60)
        report.append("")

        return "\n".join(report)

    def _calculate_grade(self) -> str:
        """Calculate overall system grade"""
        if self.metrics['critical_issues'] >= 3:
            return "D (Not Production Ready)"
        elif self.metrics['critical_issues'] >= 1:
            return "C (Needs Security Hardening)"
        elif self.metrics['failed'] > self.metrics['passed'] / 2:
            return "C+ (Functional but needs work)"
        elif self.metrics['failed'] > 0:
            return "B (Good with minor issues)"
        else:
            return "A (Production Ready)"

    def generate_json_report(self, output_path: str):
        """Generate JSON report for automated processing"""
        report = {
            'timestamp': datetime.now().isoformat(),
            'metrics': self.metrics,
            'vulnerabilities': self.vulnerabilities,
            'breaking_points': self.breaking_points,
            'test_results': self.test_results,
            'grade': self._calculate_grade()
        }

        with open(output_path, 'w') as f:
            json.dump(report, f, indent=2)

        print(f"JSON report saved to: {output_path}")

    def generate_html_dashboard(self, output_path: str):
        """Generate HTML dashboard with visualizations"""
        html = f"""
<!DOCTYPE html>
<html>
<head>
    <title>AutomationOS Stress Test Dashboard</title>
    <style>
        body {{
            font-family: 'Segoe UI', Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background: #f5f5f5;
        }}
        .container {{
            max-width: 1200px;
            margin: 0 auto;
            background: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }}
        h1 {{
            color: #333;
            border-bottom: 3px solid #007acc;
            padding-bottom: 10px;
        }}
        .metric-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 20px;
            margin: 20px 0;
        }}
        .metric-card {{
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            border-radius: 8px;
            text-align: center;
        }}
        .metric-card.critical {{
            background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%);
        }}
        .metric-card.success {{
            background: linear-gradient(135deg, #4facfe 0%, #00f2fe 100%);
        }}
        .metric-value {{
            font-size: 2.5em;
            font-weight: bold;
            margin: 10px 0;
        }}
        .metric-label {{
            font-size: 0.9em;
            opacity: 0.9;
        }}
        .vulnerability-list {{
            margin: 20px 0;
        }}
        .vuln-item {{
            padding: 15px;
            margin: 10px 0;
            border-left: 4px solid #ff6b6b;
            background: #fff5f5;
            border-radius: 4px;
        }}
        .vuln-item.warning {{
            border-left-color: #ffa500;
            background: #fff9f0;
        }}
        .grade {{
            font-size: 3em;
            text-align: center;
            padding: 30px;
            margin: 20px 0;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border-radius: 10px;
        }}
        .timestamp {{
            text-align: right;
            color: #666;
            font-size: 0.9em;
        }}
    </style>
</head>
<body>
    <div class="container">
        <h1>🔥 AutomationOS Stress Test Dashboard</h1>
        <p class="timestamp">Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>

        <div class="metric-grid">
            <div class="metric-card success">
                <div class="metric-label">Tests Passed</div>
                <div class="metric-value">{self.metrics['passed']}</div>
            </div>
            <div class="metric-card critical">
                <div class="metric-label">Tests Failed</div>
                <div class="metric-value">{self.metrics['failed']}</div>
            </div>
            <div class="metric-card critical">
                <div class="metric-label">Critical Issues</div>
                <div class="metric-value">{self.metrics['critical_issues']}</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Warnings</div>
                <div class="metric-value">{self.metrics['warnings']}</div>
            </div>
        </div>

        <div class="grade">
            Overall Grade: {self._calculate_grade()}
        </div>

        <h2>🔴 Critical Vulnerabilities</h2>
        <div class="vulnerability-list">
"""

        # Add vulnerabilities
        critical_vulns = [v for v in self.vulnerabilities if v['severity'] == 'CRITICAL']
        if critical_vulns:
            for vuln in critical_vulns[:5]:
                html += f'            <div class="vuln-item">{vuln["description"]}</div>\n'
        else:
            html += '            <div class="vuln-item" style="border-left-color: #4caf50; background: #f0fff0;">No critical vulnerabilities found!</div>\n'

        html += """
        </div>

        <h2>📊 Test Summary</h2>
        <p>For detailed analysis, see <code>STRESS_TEST_REPORT.md</code></p>

        <h2>🔧 Next Steps</h2>
        <ol>
            <li>Fix all critical vulnerabilities (Priority 1)</li>
            <li>Address failed tests (Priority 2)</li>
            <li>Implement recommended mitigations</li>
            <li>Re-run stress tests to verify fixes</li>
            <li>Enable continuous chaos testing</li>
        </ol>
    </div>
</body>
</html>
"""

        with open(output_path, 'w') as f:
            f.write(html)

        print(f"HTML dashboard saved to: {output_path}")

def main():
    print("AutomationOS Stress Test Dashboard")
    print("=" * 60)
    print()

    analyzer = StressTestAnalyzer()
    analyzer.analyze_all_logs()

    # Generate text report
    summary = analyzer.generate_summary_report()
    print(summary)

    # Save reports
    with open('stress_test_summary.txt', 'w') as f:
        f.write(summary)
    print("Text report saved to: stress_test_summary.txt")

    # Generate JSON report
    analyzer.generate_json_report('stress_test_results.json')

    # Generate HTML dashboard
    analyzer.generate_html_dashboard('stress_test_dashboard.html')

    print()
    print("=" * 60)
    print("All reports generated successfully!")
    print("Open stress_test_dashboard.html in your browser for visual report.")

if __name__ == '__main__':
    main()
