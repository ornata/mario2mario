"""Exposes the absolute path of the local ROM image to BUILD files.

The ROM must never be a Bazel source or enter git history, so it is not
a label. This repository rule only records where the ROM lives on this
machine: <workspace>/rom/mario64usa.z64, or $M2M_ROM when set (passed as
--repo_env=M2M_ROM=...). Targets pass ROM_PATH to tests through `env`.
"""

def _rom_path_impl(rctx):
    path = rctx.os.environ.get("M2M_ROM", "")
    if not path:
        path = str(rctx.workspace_root) + "/rom/mario64usa.z64"
    rctx.file("BUILD.bazel", "")
    rctx.file("defs.bzl", "ROM_PATH = %r\n" % path)

rom_path = repository_rule(
    implementation = _rom_path_impl,
    environ = ["M2M_ROM"],
    local = True,
)
