# Docker Assets

This directory holds repo-local Docker examples and sample inputs.

- The canonical image build now uses the root `Dockerfile`.
- `test.conf` is the sample solver input used in the older container notes.
- `runcommands.txt` collects a few working build and run shapes for both `kangaroo` and `kangaroo-perf`.

Typical build:

```bash
docker build --no-cache --build-arg CCAP=61 --build-arg BRANCH=testHarness -t kangaroo:testHarness .
```

The `BRANCH` build argument is retained for command-line compatibility, but the image is built from the current local checkout rather than cloning a remote branch.
