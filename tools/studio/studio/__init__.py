# SPDX-License-Identifier: MIT
"""Kiln Studio's server package: auth, jobs, and the project model it serves.

Standard library only, on purpose — the studio runs from `nix run .#studio` on
python3Minimal, the same interpreter the repository's other local web tools use.
"""
