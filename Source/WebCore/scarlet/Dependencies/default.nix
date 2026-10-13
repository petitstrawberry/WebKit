{ pkgs, runtime }:
let
  lib = pkgs.lib;
  lock = builtins.fromJSON (builtins.readFile ./dependencies.lock.json);
  sourceFor = item:
    let s = item.source;
    in if s.kind == "git" then
      let
        fetch = url: rev:
          let p = builtins.match "https://github.com/([^/]+)/([^/]+)" url;
          in (builtins.fetchTree {
            type = "github";
            owner = builtins.elemAt p 0;
            repo = lib.removeSuffix ".git" (builtins.elemAt p 1);
            inherit rev;
          }).outPath;
        source = fetch s.git_url s.commit;
      in if (s.submodules or []) == [] then source else
        pkgs.runCommand "${item.name}-source" {} ''
          mkdir -p "$out"
          cp -R ${source}/. "$out/"
          chmod -R u+w "$out"
          ${lib.concatMapStringsSep "\n" (sub: ''
            rm -rf "$out/${sub.path}"
            cp -R ${fetch sub.git_url sub.commit} "$out/${sub.path}"
          '') s.submodules}
        ''
    else pkgs.fetchurl {
      url = s.url;
      sha256 = s.sha256;
    };
  sources = builtins.listToAttrs (map (item: {
    name = item.name;
    value = sourceFor item;
  }) lock.dependencies);
  sourceMap = pkgs.writeText "scarlet-webkit-dependency-sources.json" (builtins.toJSON sources);
  icuData = pkgs.fetchurl {
    url = lock.icu_data.url;
    sha256 = lock.icu_data.sha256;
  };
  clang = pkgs.llvmPackages_21.clang-unwrapped;
  llvm = pkgs.llvmPackages_21.llvm;
in pkgs.stdenv.mkDerivation {
  pname = "scarlet-webkit-dependencies";
  version = "1";
  src = ./.;
  nativeBuildInputs = [
    pkgs.python3 pkgs.cmake pkgs.ninja pkgs.meson pkgs.pkg-config
    pkgs.perl pkgs.gperf pkgs.tcl clang llvm pkgs.llvmPackages_21.lld
  ];
  dontConfigure = true;
  dontFixup = true;
  buildPhase = ''
    runHook preBuild
    python3 build.py --sdk ${runtime} --prefix "$out" --build-dir "$PWD/build" \
      --source-map ${sourceMap} --icu-data ${icuData} \
      --cc ${clang}/bin/clang --cxx ${clang}/bin/clang++ \
      --host-cc ${pkgs.stdenv.cc}/bin/cc --host-cxx ${pkgs.stdenv.cc}/bin/c++ \
      --ar ${llvm}/bin/llvm-ar --ranlib ${llvm}/bin/llvm-ranlib \
      --strip ${llvm}/bin/llvm-strip --jobs "$NIX_BUILD_CORES"
    runHook postBuild
  '';
  installPhase = "true";
  passthru = { inherit sources; };
  meta.description = "Static AArch64 Scarlet dependencies for WebKit";
}
