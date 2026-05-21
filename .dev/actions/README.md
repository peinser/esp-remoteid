# peinser/actions

Reusable GitHub Actions workflows. Copy this directory's contents into the `peinser/actions` repository.

## Publishing

```sh
# From the peinser/actions repository root:
git add .
git commit -m "Add esp-idf-build reusable workflow"
git tag v1
git push origin main v1
```

Callers reference the workflow as:

```
peinser/actions/.github/workflows/esp-remoteid-build.yml@v1
```

---
