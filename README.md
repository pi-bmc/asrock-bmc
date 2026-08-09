# asrock-bmc — OpenBMC for the ASRock Rack X570D4I-2T

A minimal, self-contained OpenBMC build for the **ASRock Rack X570D4I-2T**
(Aspeed AST2500 BMC).

This repository contains **only the files we own** — the `meta-x570d4i`
machine layer. Every upstream layer (openembedded-core, bitbake,
meta-openembedded, meta-phosphor, meta-aspeed, and the base meta-asrock layer)
is fetched, unmodified, from upstream OpenBMC by [kas](https://kas.readthedocs.io).

```
asrock-bmc/
├── .config.yaml        # the whole build definition: pins + layer list + local.conf
└── meta-x570d4i/     # the only layer we maintain
```

## How it works

The modern `openbmc/openbmc` repository bundles openembedded-core (`meta`),
bitbake, and meta-openembedded inside itself as the in-tree `upstream-layers`
subtree. So a *single* upstream clone supplies every layer this board needs:

| Layer                              | Source                          |
| ---------------------------------- | ------------------------------- |
| `meta` (openembedded-core)         | openbmc/openbmc `upstream-layers` |
| `meta-openembedded/meta-oe`        | openbmc/openbmc `upstream-layers` |
| `meta-openembedded/meta-networking`| openbmc/openbmc `upstream-layers` |
| `meta-openembedded/meta-python`    | openbmc/openbmc `upstream-layers` |
| `meta-phosphor`                    | openbmc/openbmc                 |
| `meta-aspeed`                      | openbmc/openbmc                 |
| `meta-asrock` (base + meta-common) | openbmc/openbmc                 |
| **`meta-x570d4i`**               | **this repo**                   |

`openbmc/openbmc` is pinned to a fixed commit in [.config.yaml](.config.yaml) so the build
is reproducible and byte-for-byte equivalent to building `MACHINE=x570d4i2t`
inside a full openbmc tree at that commit.

## Building

```sh
pipx install kas     # or: pip install kas
kas build
```

kas reads [.config.yaml](.config.yaml) from the working directory automatically,
so no config argument is needed.

The image lands in `build/tmp/deploy/images/x570d4i2t/`.

### Interactive / bitbake shell

```sh
kas shell
# then, e.g.:
bitbake obmc-phosphor-image
```

## Updating upstream

Bump the `commit:` under the `openbmc` repo in [.config.yaml](.config.yaml) to a newer
`openbmc/openbmc` revision, then rebuild and re-test. Nothing else moves —
the upstream layers travel together with that single pin.
# asrock-bmc
