"""`gateway` package shim for the published repo.

The original plugin layout is a flat directory installed by Hermes as the ``gadget``
plugin. This repo mirrors it under ``gateway/`` so the device firmware and its hub live
side by side. Modules use relative imports (``from . import ota``) plus an optional
``agent`` dependency (Hermes core, injected at plugin load time), so:

- Inside Hermes: copy ``gateway/`` into ``plugins/`` as ``gadget`` — imports resolve
  exactly as documented in docs/GATEWAY.md.
- Standalone smoke tests: ``python3 -c "import sys; sys.path.insert(0, 'gateway');
  import protocol"`` works; adapter/hub additionally need a host ``agent`` package
  (they are the Hermes-coupled half and are exercised by the running gateway).

See docs/GATEWAY.md for the full install/pairing guide.
"""