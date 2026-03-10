{
  description = "Spine -- multithreaded SNMP poller for Cacti";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      packages = forAll (pkgs: rec {
        cacti-spine = pkgs.stdenv.mkDerivation {
          pname = "cacti-spine";
          version = "1.3.0";

          src = ./.;

          nativeBuildInputs = [
            pkgs.autoconf
            pkgs.automake
            pkgs.libtool
            pkgs.help2man
          ];

          buildInputs = [
            pkgs.mariadb.client
            pkgs.net-snmp
            pkgs.openssl
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            /* libcap for --enable-lcap; Linux-only (no setcap on Darwin) */
            pkgs.libcap
          ];

          preConfigure = "./bootstrap";

          configureFlags = [
            "--with-results-buffer=2048"
            "--with-max-scripts=20"
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            "--enable-lcap"
          ];

          postInstall = ''
            install -D -m 0644 spine.conf.dist $out/etc/spine.conf.dist
          '';

          meta = with pkgs.lib; {
            description = "Multithreaded SNMP poller for Cacti";
            homepage = "https://www.cacti.net/";
            license = licenses.lgpl21Plus;
            platforms = platforms.unix;
            mainProgram = "spine";
          };
        };

        default = cacti-spine;
      });

      /* nix develop -- drops into a shell with all build deps available */
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.cacti-spine ];
        };
      });
    };
}
