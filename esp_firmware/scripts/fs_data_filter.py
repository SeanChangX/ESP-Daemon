Import("env")

import shutil
from pathlib import Path
from SCons.Script import COMMAND_LINE_TARGETS

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}
if FS_TARGETS.isdisjoint(set(COMMAND_LINE_TARGETS)):
    # Only filter data files when building/uploading filesystem images.
    pass
else:
    ENV_EXCLUDES = {
        "seeed_xiao_esp32c3_prod": {
            "estop.html",
            "estop.js",
        },
        "seeed_xiao_esp32c3_estop": {
            "index.html",
            "script.js",
            "settings.html",
            "settings.js",
        },
    }

    pioenv = env.get("PIOENV", "")
    excluded = ENV_EXCLUDES.get(pioenv)
    if not excluded:
        pass
    else:
        project_data_dir = Path(env.subst("$PROJECT_DIR")) / "data"
        if not project_data_dir.exists():
            pass
        else:
            staging_dir = Path(env.subst("$BUILD_DIR")) / "data_filtered"
            if staging_dir.exists():
                shutil.rmtree(staging_dir)
            staging_dir.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(project_data_dir, staging_dir)

            for rel in excluded:
                target = staging_dir / rel
                if target.is_file() or target.is_symlink():
                    target.unlink()
                elif target.is_dir():
                    shutil.rmtree(target, ignore_errors=True)

            env.Replace(PROJECT_DATA_DIR=staging_dir.as_posix())
            print("Using filtered data dir for {}: {}".format(pioenv, staging_dir))
