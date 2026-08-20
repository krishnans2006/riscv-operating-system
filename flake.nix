{
  description = "Flake-based development environment for ECE 391 - Computer Systems Engineering";
  # Adapted from https://github.com/RoshanAH/riscv-qemu-toolchain

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/5df43628fdf08d642be8ba5b3625a6c70731c19c";
    flake-parts.url = "github:hercules-ci/flake-parts";
    # nixpkgs with the correct version of qemu so we don't have to package it ourselves
    # https://github.com/NixOS/nixpkgs/blob/81dcfeef771d77f0bc5cd8bfe01def33e7839fa9/pkgs/applications/virtualization/qemu/default.nix
    nixpkgs-qemu.url = "github:nixos/nixpkgs/5629520edecb69630a3f4d17d3d33fc96c13f6fe";
  };

  outputs =
    inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [ ];
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin" ];
      perSystem =
        { config, self', inputs', pkgs, system, ... }:
        {
          packages = {
            qemu =
              let
                inherit (inputs'.nixpkgs-qemu.legacyPackages) qemu;
              in
              (qemu.override (oldAttrs: {
                hostCpuTargets = (oldAttrs.hostCpuTargets or [ ]) ++ [
                  "riscv32-softmmu"
                  "riscv64-softmmu"
                ];
              })).overrideAttrs
                (oldAttrs: {
                  patches = oldAttrs.patches ++ [ ./qemu.patch ];
                  dontStrip = true;
                  stripDebug = false;
                  configureFlags = oldAttrs.configureFlags ++ [
                    "--disable-werror"
                    "--enable-debug"
                    "--enable-debug-info"
                    "--enable-system"
                  ];
                });
            riscv-gnu-toolchain = pkgs.callPackage ./riscv-gnu-toolchain.nix { };
          };

          devShells.default = pkgs.mkShell {
            packages = [
              self'.packages.qemu
              self'.packages.riscv-gnu-toolchain

              pkgs.screen

              pkgs.clang-tools
              pkgs.bear
            ];

            shellHook = ''
              cat <<EOF > ./.clangd
              CompileFlags:
                Remove: [-mno-riscv-attribute]
                Add:
                  - --target=riscv64-unknown-elf
                  - -nostdinc  # Or else, memory.h is pulled from glibc instead of the path below
                  - -I${self'.packages.riscv-gnu-toolchain}/lib/gcc/riscv64-unknown-elf/13.2.0/include
                  - -I${self'.packages.riscv-gnu-toolchain}/riscv64-unknown-elf/include
              EOF
            '';
          };
        };
      flake = { };
    };
}
