# Compatibility wrapper for `nix-build`, delegated to flake.nix.
# This still requires a Nix installation with flakes support.
#
# Usage:
#   nix-build default.nix   # Build the toolchain package
#   nix-shell                # Enter the default dev shell (uses shell.nix)

let
  flake = builtins.getFlake (toString ./.);
in
flake.packages.${builtins.currentSystem}.milkv-toolchain
