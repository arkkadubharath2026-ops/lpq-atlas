"""Training, evaluation and dataset tooling for the LPQ project.

Nothing in this package ever runs inside a timed region. The shortest-path hot
path is native C++ (CLAUDE.md, ESD.md section 3); Python prepares inputs,
analyses outputs, and orchestrates.
"""

__all__ = ["data", "utilities"]
