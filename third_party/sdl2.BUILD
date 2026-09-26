# Homebrew SDL2 (see MODULE.bazel). Headers are included as <SDL.h>.
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "sdl2",
    hdrs = glob(["include/SDL2/*.h"]),
    defines = ["_THREAD_SAFE"],
    includes = ["include/SDL2"],
    linkopts = [
        "-L/opt/homebrew/opt/sdl2/lib",
        "-lSDL2",
        "-Wl,-rpath,/opt/homebrew/opt/sdl2/lib",
    ],
    visibility = ["//visibility:public"],
)
