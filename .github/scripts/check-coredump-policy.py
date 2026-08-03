#!/usr/bin/env python3
"""Verify that fork pull requests cannot access or upload host coredumps."""

from pathlib import Path


WORKFLOW_PATH = Path(__file__).resolve().parents[1] / "workflows" / "linux_amd64_build.yaml"
HOST_COREDUMP_MOUNT = "--mount type=bind,source=/var/lib/systemd/coredump,target=/cores"
TRUSTED_SOURCE_CONDITION = (
    "github.event_name != 'pull_request' || "
    "github.event.pull_request.head.repo.full_name == github.repository"
)
TRUSTED_MOUNT_EXPRESSION = (
    "${{ (" + TRUSTED_SOURCE_CONDITION + ") && '" + HOST_COREDUMP_MOUNT + "' || '' }}"
)
TRUSTED_CORE_UPLOAD_CONDITION = "${{ failure() && (" + TRUSTED_SOURCE_CONDITION + ") }}"
EXPECTED_HOST_MOUNT_COUNT = 1
EXPECTED_CORES_REFERENCE_COUNT = 2
CONTAINER_MARKER = "    container:\n"
CONTAINER_END_MARKER = "    env:\n"
LOG_UPLOAD_MARKER = "      - name: Upload logs from failed tests\n"
CORE_UPLOAD_MARKER = "      - name: Upload core files from failed tests\n"
CORE_UPLOAD_END_MARKER = "      - name: Check CPU Features\n"


def extract_block(contents: str, start_marker: str, end_marker: str) -> str:
    """Return the workflow text between two unique markers."""
    start = contents.index(start_marker)
    end = contents.index(end_marker, start)
    return contents[start:end]


def normalize_whitespace(contents: str) -> str:
    """Collapse YAML formatting so policy expressions can be compared reliably."""
    return " ".join(contents.split())


def require(fragment: str, contents: str, message: str) -> None:
    """Raise an actionable error when a required policy fragment is absent."""
    if fragment not in contents:
        raise RuntimeError(message)


def require_count(fragment: str, expected: int, contents: str, message: str) -> None:
    """Raise an actionable error when a policy fragment count is unexpected."""
    actual = contents.count(fragment)
    if actual != expected:
        raise RuntimeError(f"{message} Expected {expected}, found {actual}.")


def main() -> None:
    """Validate the SEC-47 workflow trust boundary."""
    workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
    container = normalize_whitespace(extract_block(workflow, CONTAINER_MARKER, CONTAINER_END_MARKER))
    log_upload = normalize_whitespace(extract_block(workflow, LOG_UPLOAD_MARKER, CORE_UPLOAD_MARKER))
    core_upload = normalize_whitespace(extract_block(workflow, CORE_UPLOAD_MARKER, CORE_UPLOAD_END_MARKER))

    require_count(
        HOST_COREDUMP_MOUNT,
        EXPECTED_HOST_MOUNT_COUNT,
        workflow,
        "The workflow must contain exactly one host coredump mount.",
    )
    require(
        TRUSTED_MOUNT_EXPRESSION,
        container,
        "The only host coredump mount must be guarded against fork pull requests.",
    )
    require_count(
        "/cores",
        EXPECTED_CORES_REFERENCE_COUNT,
        workflow,
        "Only the guarded mount and guarded artifact may reference /cores.",
    )
    if "/cores" in log_upload:
        raise RuntimeError("The general failure-log artifact must not contain /cores.")
    require(
        TRUSTED_CORE_UPLOAD_CONDITION,
        core_upload,
        "The core artifact must be skipped for fork pull requests.",
    )
    require("path: /cores", core_upload, "The trusted core artifact must upload /cores.")

    print("SEC-47 coredump isolation policy validated")


if __name__ == "__main__":
    main()
