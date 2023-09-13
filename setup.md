# How to setup

clone the repo:

```bash
git clone $(REPO_URL)
git submodule init
git submodule update
```

add needed tools:

NOTE: only tested on Arch Linux(Or Manjaro), use your package manager instead.

```bash
pacman -S cmake ninja mold ccache
```

build mlir:

```bash
./build-mlir.sh
```

build circt:

```bash
./build-circt.sh
```

If you hacked some code, just rerun the build scipt.
