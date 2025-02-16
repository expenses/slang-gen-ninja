{
  description = "Flake utils demo";

  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system};
      dbg-macro = with pkgs; stdenv.mkDerivation {
        name = "dbg-macro";
        src = fetchGit {
        url = "https://github.com/sharkdp/dbg-macro";
        rev = "1aaa8805ca0a4852c009805e49074cc3dedf058e";
      };
      buildPhase = ''
          mkdir -p $out
          ln -s $src $out/include
        '';
      };
      in
      {
        devShells.default = with pkgs;
          mkShell {
            nativeBuildInputs = [cmake ninja gdb clang-tools];
            buildInputs = [shader-slang argparse dbg-macro hotspot linuxPackages_latest.perf];
          };
        packages.default = with pkgs; stdenv.mkDerivation {
          name = "slang-gen-ninja";
          nativeBuildInputs = [cmake];
          buildInputs = [shader-slang argparse];
          src = lib.cleanSource ./.;
          installPhase = ''
            mkdir -p $out/bin
            cp slang-gen-ninja $out/bin
          '';
        };
        packages.slang = pkgs.shader-slang;
      }
    );
}
