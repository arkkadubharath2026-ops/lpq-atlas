"""The dataset pipeline (ESD.md section 26).

    download -> validate -> normalize -> convert to CSR -> compute metadata
    -> generate query manifest -> store checksum

Raw datasets are never modified in place. See ``pipeline.prepare_dataset`` for
the whole sequence and ``acquire`` for how immutability is enforced.
"""

from lpq_tools.data.acquire import AcquisitionError, acquire_dataset, decompress
from lpq_tools.data.binary_header import BinaryGraphError, read_binary_graph_header
from lpq_tools.data.pipeline import (
    PipelineError,
    PreparationResult,
    prepare_all,
    prepare_dataset,
    summarise,
)
from lpq_tools.data.queries import (
    QueryManifest,
    QueryPair,
    build_manifest,
    generate_query_pairs,
    read_manifest,
    write_manifest,
)
from lpq_tools.data.sources import (
    DatasetConfig,
    DatasetConfigError,
    load_all_dataset_configs,
    load_dataset_config,
    parse_dataset_config,
)
from lpq_tools.data.validate import ValidationReport, validate_raw_file

__all__ = [
    "AcquisitionError",
    "BinaryGraphError",
    "DatasetConfig",
    "DatasetConfigError",
    "PipelineError",
    "PreparationResult",
    "QueryManifest",
    "QueryPair",
    "ValidationReport",
    "acquire_dataset",
    "build_manifest",
    "decompress",
    "generate_query_pairs",
    "load_all_dataset_configs",
    "load_dataset_config",
    "parse_dataset_config",
    "prepare_all",
    "prepare_dataset",
    "read_binary_graph_header",
    "read_manifest",
    "summarise",
    "validate_raw_file",
    "write_manifest",
]
