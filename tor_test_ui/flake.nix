{
  description = "tor_test_ui — dev harness for the tor module (Ready / Fetch / Host / Pair)";
  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    tor.url = "path:../tor";
  };
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
