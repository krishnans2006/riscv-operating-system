# Adapted from https://github.com/RoshanAH/riscv-qemu-toolchain

{ fetchFromGitHub, fetchgit, stdenv, curl, texinfo, bison, flex, gmp, mpfr, libmpc, python3, perl, flock, expat }:

let
  # , nix-prefetch-git --url https://sourceware.org/git/binutils-gdb.git --rev c7f28aad0c99d1d2fec4e52ebfa3735d90ceb8e9
  binutilsSrc = fetchgit {
    url = "https://sourceware.org/git/binutils-gdb.git";
    rev = "c7f28aad0c99d1d2fec4e52ebfa3735d90ceb8e9"; # v2.42
    hash = "sha256-uCeNk6eIk1G2YyohCZEF04buMzu7boPt3k2nQbdkxqU=";
  };
  # , nix-prefetch-git --url https://gcc.gnu.org/git/gcc.git --rev c891d8dc23e1a46ad9f3e757d09e57b500d40044
  gccSrc = fetchgit {
    url = "https://gcc.gnu.org/git/gcc.git";
    rev = "c891d8dc23e1a46ad9f3e757d09e57b500d40044"; # 13.2.0
    hash = "sha256-AAu/jE3MlMgvd+xagn9ujJ8PpKJZ16iZXhU9QxNRZSk=";
  };
  # , nix-prefetch-git --url https://sourceware.org/git/glibc.git --rev ae37d06c7d127817ba43850f0f898b793d42aea7
  glibcSrc = fetchgit {
    url = "https://sourceware.org/git/glibc.git";
    rev = "ae37d06c7d127817ba43850f0f898b793d42aea7"; # 2.34
    hash = "sha256-NPxYWmTuR3Fl5ak+SnxXIUDhGeNitktCr4Om54a3pe8=";
  };
  # , nix-prefetch-git --url https://sourceware.org/git/binutils-gdb.git --rev 6bda1c19bcd16eff8488facb8a67d52a436f70e7
  gdbSrc = fetchgit {
    url = "https://sourceware.org/git/binutils-gdb.git";
    rev = "6bda1c19bcd16eff8488facb8a67d52a436f70e7"; # 14.1
    hash = "sha256-ghCWNqiYyp8NdNlMfYG5opZgU+PzYk/PiF8HT/aPr2o=";
  };
  newlibSrc = fetchgit {
    url = "https://sourceware.org/git/newlib-cygwin.git";
    rev = "bf94b87f54de862a1c2482d411a18973b29264fe";
    hash = "sha256-tSYZfc8AM3fg6BhJYM8LqfWU5s0kpmRLHFZJtokpJXc=";
  };
in
stdenv.mkDerivation rec {
  pname = "riscv-gnu-toolchain";
  version = "2024.12.16";
  srcs = (
    fetchFromGitHub {
      owner = "riscv-collab";
      repo = pname;
      rev = version;
      sha256 = "sha256-FZE7DIW+aP5mAmmWdgMXohOhMLngQrG2zoyF+zV97+A=";
    }
  );

  postUnpack = ''
    copy() {
      cp -pr --reflink=auto -- "$1" "$2"
    }

    rm -r $sourceRoot/{binutils,gcc,glibc,gdb,newlib}

    copy ${binutilsSrc} $sourceRoot/binutils
    copy ${gccSrc} $sourceRoot/gcc
    copy ${glibcSrc} $sourceRoot/glibc
    copy ${gdbSrc} $sourceRoot/gdb
    copy ${newlibSrc} $sourceRoot/newlib

    chmod -R u+w -- "$sourceRoot"
  '';

  nativeBuildInputs = [
    curl
    perl
    python3
    texinfo
    bison
    flex
    gmp
    mpfr
    libmpc

    flock # required for installing file
    expat # glibc
  ];

  enableParallelBuilding = true;

  configureFlags = [
    "--enable-multilib"
  ];

  postConfigure = ''
    # nixpkgs will set those value to bare string "ar", "objdump"...
    # however we are cross-compiling, we must let $CC to determine which bintools to use.
    unset AR AS LD OBJCOPY OBJDUMP
  '';

  # RUN: make
  makeFlags = [
    # Don't auto update source
    "GCC_SRC_GIT="
    "BINUTILS_SRC_GIT="
    "GLIBC_SRC_GIT="
    "GDB_SRC_GIT="
    "NEWLIB_SRC_GIT="

    # Install to nix out dir
    "INSTALL_DIR=${placeholder "out"}"
  ];

  # -Wno-format-security
  hardeningDisable = [ "format" ];

  dontPatchELF = true;
  dontStrip = true;
}
