import os

Import("env")

pioenv = env.subst("$PIOENV")

if pioenv == "m5stack-core-esp32-16M" and os.environ.get("ALLOW_LEGACY_LARGE_SLOT") != "1":
    print()
    print("ERROR: m5stack-core-esp32-16M is the legacy large-slot layout.")
    print("Normal ProofPro hardware builds must use the OEM-compatible layout:")
    print("  pio run")
    print("  pio run -e m5stack-core-esp32-16M-oem-layout")
    print()
    print("To intentionally build the legacy layout, set:")
    print("  ALLOW_LEGACY_LARGE_SLOT=1 pio run -e m5stack-core-esp32-16M")
    env.Exit(1)
