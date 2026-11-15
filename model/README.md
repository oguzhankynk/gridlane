# Control plane model

`project.yaml` defines the control plane of gridlane: the messages between the
coordinator and the workers, their fields and ranges, and the interface that
carries them. It is a [wireforge](https://github.com/oguzhankynk/wireforge)
model. The C++ code in `../generated/` is generated from it, one library per
unit (`coordinator`, `worker`), and committed; do not edit it by hand.

`../generated/system/system_icd.md` is the interface control document of the
model, generated with the code.

## Changing a message

1. Edit `project.yaml`.
2. Regenerate: `scripts/regenerate.sh` (needs `pip install wireforge==0.5.1`).
3. Commit the model and the generated code together.

CI regenerates the code from the model and fails if it differs from what is
committed, and checks that the model is valid and in canonical layout.

## Why the interface has no framing

pairring delivers whole messages and checks each with a CRC-32C, so the
interface uses `framing: none` and no checksum of its own: every receive is
exactly one frame, and the frame starts with the 16-bit message id.

## Version

The generator version is pinned in `scripts/regenerate.sh`. Every generated
file records the generator version and a hash of the model in its header.
