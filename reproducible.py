"""Normalize checkout paths in compiler metadata for reproducible ESP32 images."""

import os

Import("env")

project_dir = os.path.abspath(env.subst("$PROJECT_DIR"))
for flag in ("-ffile-prefix-map", "-fdebug-prefix-map", "-fmacro-prefix-map"):
    env.Append(CCFLAGS=[f"{flag}={project_dir}=."])
