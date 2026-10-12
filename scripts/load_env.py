"""PlatformIO pre-build script: inject secrets from .env as compile-time defines.

Reads WEB_PASSWORD, ADMIN_PASSWORD and AP_SSID from <project>/.env (environment
variables of the same name take precedence) and adds them as string macros.
"""
import os
import sys

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

KEYS = ("WEB_PASSWORD", "ADMIN_PASSWORD", "AP_SSID")
REQUIRED = ("WEB_PASSWORD", "ADMIN_PASSWORD")


def parse_dotenv(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export "):]
            key, sep, value = line.partition("=")
            if not sep:
                continue
            key, value = key.strip(), value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            values[key] = value
    return values


project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
values = parse_dotenv(os.path.join(project_dir, ".env"))
for key in KEYS:
    if os.environ.get(key):
        values[key] = os.environ[key]

missing = [k for k in REQUIRED if not values.get(k)]
if missing:
    sys.stderr.write(
        "\n*** Missing %s. Copy .env.example to .env and set them. ***\n\n"
        % ", ".join(missing)
    )
    env.Exit(1)  # noqa: F821

if values["WEB_PASSWORD"] == values["ADMIN_PASSWORD"]:
    print("load_env: warning: WEB_PASSWORD equals ADMIN_PASSWORD; every user will be admin")

env.Append(  # noqa: F821
    CPPDEFINES=[(k, env.StringifyMacro(values[k])) for k in KEYS if values.get(k)]  # noqa: F821
)
