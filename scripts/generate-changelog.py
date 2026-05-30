#!/usr/bin/env python3
"""
Generate CHANGELOG for AutomationOS releases.

This script automatically generates a changelog by parsing git commit history
and organizing commits by type (Features, Bug Fixes, etc.).

Usage:
    python3 scripts/generate-changelog.py --version v0.1.0 --output RELEASE_NOTES.md
"""

import argparse
import subprocess
import sys
from datetime import datetime
from collections import defaultdict


class ChangelogGenerator:
    """Generate changelog from git history"""

    COMMIT_TYPES = {
        'feat': ('Features', '✨'),
        'fix': ('Bug Fixes', '🐛'),
        'docs': ('Documentation', '📚'),
        'style': ('Code Style', '💅'),
        'refactor': ('Refactoring', '♻️'),
        'perf': ('Performance', '⚡'),
        'test': ('Tests', '✅'),
        'build': ('Build System', '🔧'),
        'ci': ('CI/CD', '👷'),
        'chore': ('Chores', '🧹'),
        'revert': ('Reverts', '⏪'),
    }

    def __init__(self, version, since_tag=None):
        self.version = version
        self.since_tag = since_tag
        self.commits = defaultdict(list)

    def get_git_log(self):
        """Get git commits since last tag"""
        if self.since_tag:
            cmd = ['git', 'log', f'{self.since_tag}..HEAD', '--oneline']
        else:
            # Get last 50 commits if no tag specified
            cmd = ['git', 'log', '--oneline', '-50']

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                check=True
            )
            return result.stdout.strip().split('\n')
        except subprocess.CalledProcessError:
            print("ERROR: Failed to get git log", file=sys.stderr)
            return []

    def parse_commit(self, commit_line):
        """Parse a commit line into components"""
        parts = commit_line.split(' ', 1)
        if len(parts) != 2:
            return None, None, None

        commit_hash = parts[0]
        message = parts[1]

        # Parse conventional commit format: type(scope): message
        if ':' in message:
            prefix, description = message.split(':', 1)
            description = description.strip()

            # Extract type and scope
            if '(' in prefix and ')' in prefix:
                commit_type = prefix.split('(')[0].strip().lower()
                scope = prefix.split('(')[1].split(')')[0].strip()
            else:
                commit_type = prefix.strip().lower()
                scope = None
        else:
            # Non-conventional commit
            commit_type = 'other'
            scope = None
            description = message

        return commit_type, scope, description

    def categorize_commits(self):
        """Categorize commits by type"""
        commits = self.get_git_log()

        for commit_line in commits:
            if not commit_line.strip():
                continue

            commit_type, scope, description = self.parse_commit(commit_line)
            if commit_type and description:
                self.commits[commit_type].append({
                    'scope': scope,
                    'description': description
                })

    def generate_markdown(self):
        """Generate changelog in Markdown format"""
        lines = []

        # Header
        lines.append(f"# {self.version}")
        lines.append(f"\n*Released: {datetime.now().strftime('%Y-%m-%d')}*\n")

        # Organized by commit type
        for commit_type, (category_name, emoji) in self.COMMIT_TYPES.items():
            if commit_type in self.commits:
                lines.append(f"\n## {emoji} {category_name}\n")
                for commit in self.commits[commit_type]:
                    scope_text = f"**{commit['scope']}**: " if commit['scope'] else ""
                    lines.append(f"- {scope_text}{commit['description']}")

        # Other commits (not following conventional commits)
        if 'other' in self.commits:
            lines.append("\n## 📝 Other Changes\n")
            for commit in self.commits['other']:
                lines.append(f"- {commit['description']}")

        return '\n'.join(lines)

    def generate(self):
        """Generate the changelog"""
        self.categorize_commits()
        return self.generate_markdown()


def main():
    parser = argparse.ArgumentParser(
        description='Generate CHANGELOG for AutomationOS releases'
    )
    parser.add_argument(
        '--version',
        required=True,
        help='Version tag (e.g., v0.1.0)'
    )
    parser.add_argument(
        '--since',
        help='Previous tag to compare against'
    )
    parser.add_argument(
        '--output',
        default='RELEASE_NOTES.md',
        help='Output file (default: RELEASE_NOTES.md)'
    )

    args = parser.parse_args()

    # Generate changelog
    generator = ChangelogGenerator(args.version, args.since)
    changelog = generator.generate()

    # Write to file
    with open(args.output, 'w') as f:
        f.write(changelog)

    print(f"Changelog written to {args.output}")
    print("\nPreview:")
    print("=" * 50)
    print(changelog[:500] + "...")


if __name__ == '__main__':
    main()
