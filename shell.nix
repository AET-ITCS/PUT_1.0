# shell.nix compatibility wrapper - delegates to flake.nix.
# This still requires a Nix installation with flakes support; prefer
# `nix develop` when possible.
#
# Usage:
#   nix-shell                        # Enter the default dev shell
#   nix-shell --argstr shell minimal # Enter minimal dev shell

{ shell ? "default" }:

let
  flake = builtins.getFlake (toString ./.);
in
flake.devShells.${builtins.currentSystem}.${shell}
