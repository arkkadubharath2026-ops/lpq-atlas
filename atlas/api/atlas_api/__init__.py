"""LPQ Atlas web orchestration layer.

The research tooling in ``python/lpq_tools`` is reused rather than copied
(spec 4.6). Its directory is put on the import path here, in the package
initialiser, because this runs before any submodule -- an import placed in a
submodule would depend on import order, which formatters are free to change.
"""

import sys
from pathlib import Path

_PYTHON_TOOLS = Path(__file__).resolve().parents[3] / "python"
if str(_PYTHON_TOOLS) not in sys.path:
    sys.path.insert(0, str(_PYTHON_TOOLS))
