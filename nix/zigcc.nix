# arm-linux-musleabihf toolchain wrappers around zig (bundled musl), static by default
{ pkgs }:
let
  llvm = pkgs.llvmPackages_latest;
  t = "arm-linux-musleabihf";
  wrap = name: body: pkgs.writeShellScriptBin "${t}-${name}" ''
    export ZIG_GLOBAL_CACHE_DIR=''${ZIG_GLOBAL_CACHE_DIR:-''${TMPDIR:-/tmp}/zig-cache}
    export ZIG_LOCAL_CACHE_DIR=''${ZIG_LOCAL_CACHE_DIR:-$ZIG_GLOBAL_CACHE_DIR}
    ${body}
  '';
  # drop linker options zig's linker does not implement (busybox's trylink
  # adds them for its link map / diagnostics only)
  cc = ''
    args=()
    for a in "$@"; do
      case "$a" in
        -Wl,--warn-common|-Wl,-Map,*|-Wl,--verbose) ;;
        *) args+=("$a") ;;
      esac
    done
    exec ${pkgs.zig}/bin/zig cc -target ${t} -mcpu=cortex_a7 "''${args[@]}"
  '';
in
pkgs.symlinkJoin {
  name = "zigcc-${t}";
  paths = [
    (wrap "cc" cc)
    (wrap "gcc" cc)
    (wrap "c++" ''exec ${pkgs.zig}/bin/zig c++ -target ${t} -mcpu=cortex_a7 "$@"'')
    (wrap "ar" ''exec ${llvm.llvm}/bin/llvm-ar "$@"'')
    (wrap "ranlib" ''exec ${llvm.llvm}/bin/llvm-ranlib "$@"'')
    (wrap "nm" ''exec ${llvm.llvm}/bin/llvm-nm "$@"'')
    (wrap "strip" ''exec ${llvm.llvm}/bin/llvm-strip "$@"'')
    (wrap "objcopy" ''exec ${llvm.llvm}/bin/llvm-objcopy "$@"'')
    (wrap "objdump" ''exec ${llvm.llvm}/bin/llvm-objdump "$@"'')
  ];
}
