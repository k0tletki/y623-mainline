{
  description = "Mainline Linux + blob-free userspace for the Yi 2K Pro (Y623, Allwinner V831) camera";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      src = self;
      sources = import ./nix/sources.nix { inherit pkgs; };
      zigcc = import ./nix/zigcc.nix { inherit pkgs; };
      kernel = import ./nix/kernel.nix { inherit pkgs sources src; };
      userspace = import ./nix/userspace.nix { inherit pkgs sources src zigcc kernel; };
      model = import ./nix/model.nix { inherit pkgs sources src; };
      uboot = import ./nix/u-boot.nix { inherit pkgs sources src; };
      initramfs = import ./nix/initramfs.nix { inherit pkgs src kernel userspace; };
      sdImage = import ./nix/sd-image.nix { inherit pkgs kernel initramfs model uboot; };
    in
    {
      packages.${system} = userspace // {
        inherit kernel model uboot initramfs;
        sd-image-tool = sdImage;
        # everything for the SD card except secrets (initramfs without them)
        default = pkgs.linkFarm "y623-boot" [
          { name = "zImage"; path = "${kernel}/zImage"; }
          { name = "sun8i-v831-yi-y623.dtb"; path = "${kernel}/sun8i-v831-yi-y623.dtb"; }
          { name = "8189fs.ko"; path = "${kernel}/modules/8189fs.ko"; }
          { name = "yolov5n.nnp"; path = "${model}/yolov5n.nnp"; }
          { name = "initramfs-nosecrets.cpio.gz"; path = "${initramfs}/initramfs.cpio.gz"; }
          { name = "u-boot-v831-mmc.bin"; path = "${uboot}/u-boot-v831-mmc.bin"; }
        ];
      };

      # nix run .#sd-image -- SECRETS_DIR OUT.img
      apps.${system}.sd-image = {
        type = "app";
        program = "${sdImage}/bin/y623-sd-image";
        meta.description = "Add secrets to the initramfs and write an SD card image (or just the FAT files)";
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          llvmPackages_latest.clang-unwrapped llvmPackages_latest.lld llvmPackages_latest.llvm
          bc bison flex perl openssl elfutils zig ubootTools mtools dosfstools util-linux
          (python3.withPackages (p: with p; [ numpy onnx onnxruntime pillow ]))
        ];
      };
    };
}
