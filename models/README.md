# models

The trained models the mods use. They are not committed (see NOTICE.md for where they come from and their
licences); the build scripts copy whatever is here into the app or the patcher.

- Get the ones the current release was built with: `tools/build/fetch_inputs.sh`
- Or make them yourself with the scripts in `tools/models/`:
  - macOS (Core ML): `export_beatthis.py` → `beatthis.mlmodelc`, `htdemucs_coreml.sh` → `htdemucs.mlmodelc`,
    `effnet_coreml.py` → `effnet.mlmodelc`
  - Windows (ONNX): `make_onnx.sh` → `beatthis.onnx`, `htdemucs.onnx`, `effnet.onnx`
  - both: `export_keycnn.py` → `keycnn.bin`
