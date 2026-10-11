"""Cost-aware queue-geometry policy: training, evaluation and export.

The C++ engine (``cpp/include/lpq/lpq/queue_policy.hpp``) reads a small text
policy file and, once per graph, picks the radix heap or a pooled bucket queue of
a particular width.  This package produces that file from *measured* paired
runtimes.  Nothing here runs inside a timed region.

Deliberately empty of imports: ``python -m lpq_tools.policy.train`` must not find
``train`` already imported by the package.
"""
