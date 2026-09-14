# SPDX-License-Identifier: MIT
#
# nix/checks/studio-module.nix — nixosModules.kiln-studio, booted on nixos-25.11.
#
# The module's claims are about a running machine, so they are checked on one:
#
#   * it evaluates on nixos-25.11 (Oligarchy's release) with rootless docker and
#     a stand-in for Oligarchy's strict-egress option, which it appends to;
#   * the user unit loads the image from the store and brings the studio up
#     under the user's rootless daemon, answering on 127.0.0.1 and published
#     there only;
#   * the container has the limits, read-only root and dropped capabilities
#     the options say;
#   * Tailscale identity is believed from the proxy (a loopback client, which is
#     what `tailscale serve` is) for an allowed login only, and a foreign Host
#     is refused;
#   * the serve port is open on tailscale0 and nowhere else, and the serve unit
#     proxies to the right place;
#   * a build started inside the container runs in the host's daemon and its
#     output is visible on both sides;
#   * stopping the unit removes the container.
{ pkgs2511, kilnModule }:

pkgs2511.testers.runNixOSTest {
  name = "kiln-studio-module";

  nodes.machine = { config, pkgs, lib, ... }: {
    imports = [
      kilnModule
      # Oligarchy's strict-egress, reduced to the one option the module touches.
      ({ lib, ... }: {
        options.networking.firewall.strictEgress.allow.domains = lib.mkOption {
          type = lib.types.listOf lib.types.str;
          default = [ ];
        };
      })
    ];

    virtualisation = {
      memorySize = 4096;
      cores = 4;
      diskSize = 8192;
      # As Oligarchy configures it: 25.11's default docker_28 is marked
      # insecure and refuses to evaluate, so the maintained release is pinned.
      docker.rootless = { enable = true; setSocketVariable = true; package = pkgs.docker_29; };
    };
    users.users.alice = { isNormalUser = true; linger = true; };
    services.tailscale.enable = true;
    systemd.tmpfiles.rules = [ "d /home/alice/project 0755 alice users -" ];
    environment.systemPackages = [ pkgs.busybox pkgs.curl ];
    environment.etc."kiln-test/egress.json".text = builtins.toJSON config.networking.firewall.strictEgress.allow.domains;

    custom.kilnStudio = {
      enable = true;
      user = "alice";
      projectDir = "/home/alice/project";
      allowedLogins = [ "alice@example.com" ];
      allowedHosts = [ "machine.example.ts.net" ];
      studio = { cpus = "2"; memory = "1g"; };
      egressDomains = [ "studio.example.org" ];
    };
  };

  testScript = ''
    import json

    def as_alice(cmd):
        return machine.succeed(
            "su - alice -c 'DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock " + cmd + "'")

    machine.wait_for_unit("multi-user.target")
    machine.wait_for_unit("docker.service", "alice", timeout=600)
    machine.wait_for_unit("kiln-studio.service", "alice", timeout=900)

    with subtest("the studio answers on loopback"):
        machine.wait_until_succeeds("curl -sf http://127.0.0.1:8420/ | grep -q 'Kiln Studio'", timeout=900)

    with subtest("the container is limited and locked down as configured"):
        c = json.loads(as_alice("docker inspect kiln-studio"))[0]
        hc = c["HostConfig"]
        assert hc["NanoCpus"] == 2 * 10**9, hc["NanoCpus"]
        assert hc["Memory"] == 1 << 30, hc["Memory"]
        assert hc["ReadonlyRootfs"] is True
        assert "ALL" in (hc["CapDrop"] or []), hc["CapDrop"]
        binds = hc["PortBindings"]["8420/tcp"]
        assert [b["HostIp"] for b in binds] == ["127.0.0.1"], binds

    with subtest("tailscale identity only for an allowed login, only for an allowed host"):
        who = machine.succeed("curl -sf -H 'Host: machine.example.ts.net' "
                              "-H 'Tailscale-User-Login: alice@example.com' http://127.0.0.1:8420/api/whoami")
        assert json.loads(who) == {"user": "alice@example.com", "via": "tailscale"}, who
        machine.fail("curl -sf -H 'Host: machine.example.ts.net' "
                     "-H 'Tailscale-User-Login: mallory@example.com' http://127.0.0.1:8420/api/whoami")
        machine.fail("curl -sf -H 'Host: attacker.example' "
                     "-H 'Tailscale-User-Login: alice@example.com' http://127.0.0.1:8420/api/whoami")

    with subtest("the serve port is open on tailscale0 only, and serve proxies to the studio"):
        rules = machine.succeed("iptables-save")
        opened = [l for l in rules.splitlines() if "--dport 80 " in l and "-j nixos-fw-accept" in l]
        assert opened and all("-i tailscale0" in l for l in opened), opened
        unit = machine.succeed("systemctl cat kiln-studio-tailscale-serve.service")
        assert "serve --bg --http=80 http://127.0.0.1:8420" in unit, unit

    with subtest("the module appended to strict-egress"):
        assert "studio.example.org" in json.loads(machine.succeed("cat /etc/kiln-test/egress.json"))

    with subtest("a build inside the container runs in the host's daemon"):
        machine.succeed("""cat > /home/alice/project/probe.nix <<'EOF'
    derivation {
      name = "kiln-studio-probe";
      system = builtins.currentSystem;
      builder = "''${builtins.storePath "${pkgs2511.busybox}"}/bin/sh";
      args = [ "-c" "echo built-by-the-host > $out" ];
    }
    EOF""")
        out = as_alice("docker exec kiln-studio nix build --no-link --print-out-paths -f /home/alice/project/probe.nix").strip()
        assert out.startswith("/nix/store/"), out
        assert as_alice(f"docker exec kiln-studio cat {out}").strip() == "built-by-the-host"
        assert machine.succeed(f"cat {out}").strip() == "built-by-the-host"

    with subtest("stopping the unit removes the container"):
        machine.succeed("systemctl --machine=alice@ --user stop kiln-studio.service")
        machine.wait_until_succeeds("test -z \"$(su - alice -c 'DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock docker ps -aq --filter name=kiln-studio')\"", timeout=120)
  '';
}
