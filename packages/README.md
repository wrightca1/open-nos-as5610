# Platform packages — ONL-style layout

Directory layout mirrors [Open Network Linux (ONL)](https://github.com/opencomputeproject/OpenNetworkLinux) so we stay aligned with their open-source platform structure.

```
packages/
  platforms/
    accton/
      powerpc/
        as5610-52x/          # Accton AS5610-52X
          platform-config/
            r0/
              src/lib/
                powerpc-accton-as5610-52x-r0.yml   # Platform spec (loader, FIT, env, installer)
          README.md
```

- **Platform YAML** is the single source of truth; `onie-installer/cumulus/init/accton_as5610_52x/` and `uboot_env/` implement it.
- See `platforms/accton/powerpc/as5610-52x/README.md` for implementation mapping and ONL differences.
