{
  description = "tor — headless Tor capability module for Logos Basecamp";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      # currentSystem → impure (build with --impure). Same tradeoff node-remote/receiver
      # document: catalog CI can't auto-build, so releases propagate manually.
      system = builtins.currentSystem;
      pkgs = logos-module-builder.inputs.nixpkgs.legacyPackages.${system};
      # tor bundled at <moduleDir>/lib/bin/ so the .lgx is zero-install.
      torBundle = pkgs.callPackage ./nix/tor-bundle.nix { };
    in
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      postInstall = ''
        mkdir -p $out/lib/bin
        cp -a ${torBundle}/. $out/lib/bin/
        chmod -R u+w $out/lib/bin
      '';
    };
}
