{
  description = "godot-sandbox GDExtension binaries for Linux x86_64 and aarch64 and Windows x86_64, at single and double precision";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
  inputs.godot-cpp = { url = "github:godotengine/godot-cpp/60b5a4196de8442b43b32ba68ebe1e79cfcb762f"; flake = false; };

  outputs = { self, nixpkgs, godot-cpp }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      targets = {
        linux-x86_64 = { stdenv = pkgs.stdenv; platform = "linux"; arch = "x86_64"; };
        linux-arm64 = { stdenv = pkgs.pkgsCross.aarch64-multiplatform.stdenv; platform = "linux"; arch = "arm64"; };
        windows-x86_64 = { stdenv = pkgs.pkgsCross.mingwW64.stdenv; platform = "windows"; arch = "x86_64"; extra = "use_mingw=yes"; };
      };
      build = name: t: precision: t.stdenv.mkDerivation {
        pname = "godot-sandbox-${name}-${precision}";
        version = self.shortRev or "dirty";
        src = pkgs.lib.cleanSourceWith {
          src = self;
          filter = path: type: !(builtins.elem (baseNameOf path) [ "flake.nix" "flake.lock" ".github" ]);
        };
        depsBuildBuild = [ pkgs.scons pkgs.python3 ];
        dontConfigure = true;
        postUnpack = ''
          rm -rf $sourceRoot/ext/godot-cpp
          cp -r ${godot-cpp} $sourceRoot/ext/godot-cpp
          chmod -R u+w $sourceRoot/ext/godot-cpp
        '';
        buildPhase = ''
          mkdir -p .cross
          ln -s "$(command -v $CC)" .cross/gcc
          ln -s "$(command -v $CXX)" .cross/g++
          ln -s "$(command -v $AR)" .cross/ar
          ln -s "$(command -v $RANLIB)" .cross/ranlib
          export PATH=$PWD/.cross:$PATH
          scons -j$NIX_BUILD_CORES target=template_release platform=${t.platform} arch=${t.arch} precision=${precision} ${t.extra or ""} \
            CC=$CC CXX=$CXX AR=$AR
        '';
        installPhase = ''
          mkdir -p $out
          find bin/addons/godot_sandbox/bin -maxdepth 1 -type f -name 'libgodot_riscv.*' -newer SConstruct -exec cp {} $out/ \;
          test -n "$(ls $out)"
        '';
      };
      libs = pkgs.lib.concatMapAttrs (name: t: {
        "${name}-single" = build name t "single";
        "${name}-double" = build name t "double";
      }) targets;
    in {
      overlays.default = final: prev: { godot-sandbox-addon = self.packages.${system}.addon; };
      packages.${system} = libs // {
        addon = pkgs.runCommand "godot-sandbox-addon" { } ''
          mkdir -p $out/addons/godot_sandbox/bin
          ${pkgs.lib.concatMapStrings (d: "cp ${d}/* $out/addons/godot_sandbox/bin/\n") (builtins.attrValues libs)}
          cp ${self}/bin/addons/godot_sandbox/bin/godot-riscv.gdextension $out/addons/godot_sandbox/bin/ 2>/dev/null || true
        '';
        default = self.packages.${system}.addon;
      };
    };
}
