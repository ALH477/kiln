# SPDX-License-Identifier: MIT
#
# nix/studio-images.nix — the container image Kiln Studio runs in.
#
# dockerTools, not nixos-generators (packages.dev-image): the studio is one
# process, and a NixOS init inside a rootless container fights the user
# namespace for nothing. The image has no Nix store of its own worth the name.
# nix/studio-module.nix mounts the host's /nix/store read-only over it and
# hands the host daemon's socket in, so a build the studio starts is a build on
# the host — sandboxed by the host's nix.conf, bounded by its cores/max-jobs
# rather than by this container's cgroup — and its output is visible here the
# moment it exists.
#
# That is also why `runtime` is exposed. The image tarball is compressed, so it
# carries no store references, and nothing roots the paths inside it; once the
# host's store is mounted over the image's own copy, the collector would be free
# to delete the very interpreter the container runs. The module adds `runtime`
# to the system closure, which roots all of it.
{ pkgs, lib, toolsSrc }:

let
  # The studio as deployed: its server and page, and the editors it serves.
  # Validators run from the mounted project, which is the checkout being edited.
  studioTools = lib.fileset.toSource {
    root = toolsSrc;
    fileset = lib.fileset.unions [
      (toolsSrc + "/studio")
      (toolsSrc + "/mapmaker/index.html")
      (toolsSrc + "/mapmaker/src")
      (toolsSrc + "/mapmaker/vendor")
      (toolsSrc + "/poser/index.html")
      (toolsSrc + "/poser/src")
      (toolsSrc + "/webcommon")
    ];
  };

  python = pkgs.python3Minimal;

  server = pkgs.writeShellScriptBin "kiln-studio-server" ''
    exec ${python}/bin/python3 ${studioTools}/studio/server.py "$@"
  '';

  etc = pkgs.runCommand "kiln-studio-etc" { } ''
    mkdir -p $out/etc/nix
    printf 'root:x:0:0:root:/root:/bin/bash\n' > $out/etc/passwd
    printf 'root:x:0:\n' > $out/etc/group
    printf 'experimental-features = nix-command flakes\n' > $out/etc/nix/nix.conf
  '';

  runtime = pkgs.buildEnv {
    name = "kiln-studio-runtime";
    paths = [
      server python etc
      pkgs.bashInteractive pkgs.coreutils pkgs.gnugrep pkgs.gnused pkgs.findutils
      pkgs.git pkgs.nix pkgs.cacert
    ];
    pathsToLink = [ "/bin" "/etc" ];
  };

  studio = pkgs.dockerTools.buildLayeredImage {
    name = "kiln-studio";
    contents = [ runtime ];
    extraCommands = ''
      mkdir -p tmp root nix/var/nix/daemon-socket
      chmod 1777 tmp
    '';
    config = {
      Entrypoint = [ "/bin/kiln-studio-server" ];
      Env = [
        "PATH=/bin"
        "HOME=/root"
        "NIX_REMOTE=daemon"
        "SSL_CERT_FILE=/etc/ssl/certs/ca-bundle.crt"
        "NIX_SSL_CERT_FILE=/etc/ssl/certs/ca-bundle.crt"
      ];
      ExposedPorts."8420/tcp" = { };
    };
  };
in
{
  studio = studio // { inherit runtime studioTools; };
  inherit runtime studioTools;
}
