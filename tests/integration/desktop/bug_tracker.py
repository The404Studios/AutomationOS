#!/usr/bin/env python3
"""
AutomationOS Bug Tracker
Agent 12: Integration Test Lead

Tracks bugs found during integration testing and assigns them to responsible agents.
"""

import sys
import os
import json
from datetime import datetime
from pathlib import Path


class BugTracker:
    """Bug tracking and triage system"""

    def __init__(self):
        self.bugs_file = "build/integration_bugs.json"
        self.bugs = self.load_bugs()

        # Agent assignments based on component
        self.agent_map = {
            "ipc": "Agent 1 (IPC Architect)",
            "filesystem": "Agent 2 (Filesystem Engineer)",
            "dynamic_linker": "Agent 3 (Dynamic Linker Specialist)",
            "input": "Agent 4 (Input Pipeline Developer)",
            "compositor": "Agent 5 (Framebuffer Compositor Engineer)",
            "fonts": "Agent 6 (Font Rendering Engineer)",
            "images": "Agent 7 (Image Decoder Specialist)",
            "window_manager": "Agent 8 (Window Manager Integrator)",
            "terminal": "Agent 9 (Terminal Emulator Developer)",
            "file_manager": "Agent 10 (File Manager Developer)",
            "desktop_shell": "Agent 11 (Desktop Shell Integrator)",
            "integration": "Agent 12 (Integration Test Lead)",
            "kernel": "Kernel Team",
            "boot": "Boot Team",
        }

    def load_bugs(self):
        """Load bugs from JSON file"""
        if os.path.exists(self.bugs_file):
            with open(self.bugs_file, 'r') as f:
                return json.load(f)
        return {"bugs": [], "next_id": 1}

    def save_bugs(self):
        """Save bugs to JSON file"""
        os.makedirs(os.path.dirname(self.bugs_file), exist_ok=True)
        with open(self.bugs_file, 'w') as f:
            json.dump(self.bugs, f, indent=2)

    def add_bug(self, title, description, component, severity="medium"):
        """Add a new bug"""
        bug_id = self.bugs["next_id"]
        self.bugs["next_id"] += 1

        assigned_to = self.agent_map.get(component, "Unassigned")

        bug = {
            "id": bug_id,
            "title": title,
            "description": description,
            "component": component,
            "severity": severity,  # low, medium, high, critical
            "status": "open",  # open, in_progress, resolved, closed
            "assigned_to": assigned_to,
            "created": datetime.now().isoformat(),
            "updated": datetime.now().isoformat(),
            "comments": [],
        }

        self.bugs["bugs"].append(bug)
        self.save_bugs()

        print(f"[+] Bug #{bug_id} created: {title}")
        print(f"    Component: {component}")
        print(f"    Assigned to: {assigned_to}")
        print(f"    Severity: {severity}")

        return bug_id

    def update_bug(self, bug_id, status=None, comment=None):
        """Update bug status or add comment"""
        for bug in self.bugs["bugs"]:
            if bug["id"] == bug_id:
                if status:
                    bug["status"] = status
                    bug["updated"] = datetime.now().isoformat()
                    print(f"[*] Bug #{bug_id} status updated: {status}")

                if comment:
                    bug["comments"].append({
                        "timestamp": datetime.now().isoformat(),
                        "comment": comment,
                    })
                    print(f"[*] Comment added to bug #{bug_id}")

                self.save_bugs()
                return True

        print(f"[!] Bug #{bug_id} not found")
        return False

    def list_bugs(self, status=None, component=None, severity=None):
        """List bugs with optional filters"""
        filtered = self.bugs["bugs"]

        if status:
            filtered = [b for b in filtered if b["status"] == status]
        if component:
            filtered = [b for b in filtered if b["component"] == component]
        if severity:
            filtered = [b for b in filtered if b["severity"] == severity]

        return filtered

    def generate_triage_report(self):
        """Generate bug triage report"""
        print("=" * 70)
        print("BUG TRIAGE REPORT")
        print(f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print("=" * 70)

        # Summary by status
        open_bugs = self.list_bugs(status="open")
        in_progress = self.list_bugs(status="in_progress")
        resolved = self.list_bugs(status="resolved")

        print(f"\nSummary:")
        print(f"  Open: {len(open_bugs)}")
        print(f"  In Progress: {len(in_progress)}")
        print(f"  Resolved: {len(resolved)}")
        print(f"  Total: {len(self.bugs['bugs'])}")

        # Critical/High priority bugs
        critical = self.list_bugs(severity="critical", status="open")
        high = self.list_bugs(severity="high", status="open")

        if critical:
            print(f"\n⚠️  CRITICAL BUGS ({len(critical)}):")
            for bug in critical:
                print(f"  #{bug['id']}: {bug['title']}")
                print(f"      Component: {bug['component']} | Assigned: {bug['assigned_to']}")

        if high:
            print(f"\n⚠  HIGH PRIORITY BUGS ({len(high)}):")
            for bug in high:
                print(f"  #{bug['id']}: {bug['title']}")
                print(f"      Component: {bug['component']} | Assigned: {bug['assigned_to']}")

        # Bugs by agent
        print(f"\n📋 BUGS BY AGENT:")
        agent_bugs = {}
        for bug in open_bugs + in_progress:
            agent = bug["assigned_to"]
            if agent not in agent_bugs:
                agent_bugs[agent] = []
            agent_bugs[agent].append(bug)

        for agent, bugs in sorted(agent_bugs.items()):
            print(f"\n  {agent} ({len(bugs)} bugs):")
            for bug in bugs:
                status_icon = "🔴" if bug["status"] == "open" else "🟡"
                print(f"    {status_icon} #{bug['id']}: {bug['title']} [{bug['severity']}]")

        print("\n" + "=" * 70)

    def export_markdown(self, filename="build/bugs.md"):
        """Export bugs to markdown"""
        os.makedirs(os.path.dirname(filename), exist_ok=True)

        with open(filename, 'w') as f:
            f.write("# AutomationOS Integration Bugs\n\n")
            f.write(f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
            f.write("---\n\n")

            # Group by status
            for status in ["open", "in_progress", "resolved", "closed"]:
                bugs = self.list_bugs(status=status)
                if not bugs:
                    continue

                f.write(f"## {status.replace('_', ' ').title()} ({len(bugs)})\n\n")

                for bug in bugs:
                    severity_badge = {
                        "critical": "🔴 CRITICAL",
                        "high": "🟠 HIGH",
                        "medium": "🟡 MEDIUM",
                        "low": "🟢 LOW",
                    }.get(bug["severity"], bug["severity"])

                    f.write(f"### Bug #{bug['id']}: {bug['title']}\n\n")
                    f.write(f"- **Severity:** {severity_badge}\n")
                    f.write(f"- **Component:** {bug['component']}\n")
                    f.write(f"- **Assigned:** {bug['assigned_to']}\n")
                    f.write(f"- **Created:** {bug['created']}\n")
                    f.write(f"- **Updated:** {bug['updated']}\n\n")
                    f.write(f"**Description:**\n{bug['description']}\n\n")

                    if bug["comments"]:
                        f.write("**Comments:**\n")
                        for comment in bug["comments"]:
                            f.write(f"- [{comment['timestamp']}] {comment['comment']}\n")
                        f.write("\n")

                    f.write("---\n\n")

        print(f"[*] Bugs exported to {filename}")


def main():
    """Command-line interface for bug tracker"""
    import argparse

    parser = argparse.ArgumentParser(description="AutomationOS Bug Tracker")
    subparsers = parser.add_subparsers(dest="command", help="Commands")

    # Add bug
    add_parser = subparsers.add_parser("add", help="Add a new bug")
    add_parser.add_argument("--title", required=True, help="Bug title")
    add_parser.add_argument("--description", required=True, help="Bug description")
    add_parser.add_argument("--component", required=True, help="Component")
    add_parser.add_argument("--severity", default="medium", choices=["low", "medium", "high", "critical"])

    # Update bug
    update_parser = subparsers.add_parser("update", help="Update bug status")
    update_parser.add_argument("--id", type=int, required=True, help="Bug ID")
    update_parser.add_argument("--status", choices=["open", "in_progress", "resolved", "closed"])
    update_parser.add_argument("--comment", help="Add comment")

    # List bugs
    list_parser = subparsers.add_parser("list", help="List bugs")
    list_parser.add_argument("--status", help="Filter by status")
    list_parser.add_argument("--component", help="Filter by component")
    list_parser.add_argument("--severity", help="Filter by severity")

    # Triage report
    subparsers.add_parser("triage", help="Generate triage report")

    # Export
    export_parser = subparsers.add_parser("export", help="Export bugs to markdown")
    export_parser.add_argument("--output", default="build/bugs.md", help="Output file")

    args = parser.parse_args()

    tracker = BugTracker()

    if args.command == "add":
        tracker.add_bug(args.title, args.description, args.component, args.severity)

    elif args.command == "update":
        tracker.update_bug(args.id, args.status, args.comment)

    elif args.command == "list":
        bugs = tracker.list_bugs(args.status, args.component, args.severity)
        for bug in bugs:
            print(f"#{bug['id']}: {bug['title']} [{bug['status']}] - {bug['assigned_to']}")

    elif args.command == "triage":
        tracker.generate_triage_report()

    elif args.command == "export":
        tracker.export_markdown(args.output)

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
